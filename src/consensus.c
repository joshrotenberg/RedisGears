#include "consensus.h"
#include "redisgears_memory.h"
#include "redismodule.h"
#include "utils/dict.h"
#include "utils/buffer.h"
#include "cluster.h"
#include <stdbool.h>
#include <assert.h>
#include "lock_handler.h"
#include "config.h"
#include <time.h>

Gears_dict* consensusDict;

typedef enum SendBuffPolicy{
    ONLY_MYSELF, ALL, ALL_AND_MYSENF, SPACIFIC_NODE,
}SendBuffPolicy;

typedef struct SendBuffCtx{
    Gears_Buffer *buf;
    SendBuffPolicy policy;
    char* nodeId; // in case policy is SPACIFIC_NODE
    const char* function;
}SendBuffCtx;

#define SendBuffCtx_Init(b, p, n, f) \
        (SendBuffCtx){ \
            .buf = b, \
            .policy = p, \
            .nodeId = n, \
            .function = #f, \
        }

static void Consensus_SendBuff(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    SendBuffCtx *sbctx = (*(SendBuffCtx**)payload);
    switch(sbctx->policy){
    case ONLY_MYSELF:
        Cluster_SendMsgToMySelf(sbctx->function, sbctx->buf->buff, sbctx->buf->size);
        break;
    case ALL:
        Cluster_SendMsg(NULL, sbctx->function, sbctx->buf->buff, sbctx->buf->size);
        break;
    case ALL_AND_MYSENF:
        Cluster_SendMsgToAllAndMyself(sbctx->function, sbctx->buf->buff, sbctx->buf->size);
        break;
    case SPACIFIC_NODE:
        assert(sbctx->nodeId);
        Cluster_SendMsg(sbctx->nodeId, sbctx->function, sbctx->buf->buff, sbctx->buf->size);
        break;
    default:
        assert(0);
    }
    Gears_BufferFree(sbctx->buf);
    if(sbctx->nodeId){
        RG_FREE(sbctx->nodeId);
    }
    RG_FREE(sbctx);
}

static bool Consensus_ValEquals(const char* val1, const char* val2){
    if(!val1){
        return !val2;
    }
    if(!val2){
        return !val1;
    }
    return memcmp(val1, val2, REDISMODULE_NODE_ID_LEN) == 0;
}

static char* Consensus_ValDup(const char* val, size_t len, size_t* outLen){
    *outLen = len;
    char* ret = RG_ALLOC(len);
    memcpy(ret, val, len);
    return ret;
}

static ConsensusInstance* Consensus_InstanceCreate(Consensus* consensus, long long consensusId){
    Gears_listNode* n = Gears_listFirst(consensus->consensusInstances);
    ConsensusInstance* consensusInstance = n? n->value : NULL;
    if(consensusId == -1 && consensusInstance && !consensusInstance->learner.valueLeared){
        return consensusInstance;
    }
    consensusInstance = RG_CALLOC(1, sizeof(*consensusInstance));
    if(consensusId >= 0){
        consensusInstance->consensusId = consensusId;
        if(consensusId >= consensus->currConsensusId){
            consensus->currConsensusId = consensusId + 1;
        }
    }else{
        consensusInstance->consensusId = consensus->currConsensusId++;
    }

    consensusInstance->phase = PHASE_ONE;

    if(Gears_listLength(consensus->consensusInstances) == 0){
        Gears_listAddNodeHead(consensus->consensusInstances, consensusInstance);
    }else{
        Gears_listNode* node = Gears_listFirst(consensus->consensusInstances);
        do{
            ConsensusInstance* inst = Gears_listNodeValue(node);
            assert(inst->consensusId != consensusInstance->consensusId);
            if(inst->consensusId < consensusInstance->consensusId){
                Gears_listInsertNode(consensus->consensusInstances, node, consensusInstance, 0);
                return consensusInstance;
            }
        }while((node = Gears_listNextNode(node)));
        Gears_listAddNodeTail(consensus->consensusInstances, consensusInstance);
    }

    return consensusInstance;
}

static ConsensusInstance* Consensus_InstanceGetOrCreate(Consensus* consensus, long long consensusId){
    Gears_listIter *iter = Gears_listGetIterator(consensus->consensusInstances, AL_START_HEAD);
    Gears_listNode *node = NULL;
    while((node = Gears_listNext(iter))){
        ConsensusInstance* instance = Gears_listNodeValue(node);
        if(instance->consensusId == consensusId){
            Gears_listReleaseIterator(iter);
            return instance;
        }
        if(instance->consensusId < consensusId){
            break;
        }
    }

    // not found let create one
    Gears_listReleaseIterator(iter);

    ConsensusInstance* instance = Consensus_InstanceCreate(consensus, consensusId);

    return instance;
}

static ConsensusInstance* Consensus_InstanceGet(Consensus* consensus, long long consensusId){
    Gears_listIter *iter = Gears_listGetIterator(consensus->consensusInstances, AL_START_HEAD);
    Gears_listNode *node = NULL;
    while((node = Gears_listNext(iter))){
        ConsensusInstance* instance = Gears_listNodeValue(node);
        if(instance->consensusId == consensusId){
            Gears_listReleaseIterator(iter);
            return instance;
        }
        if(instance->consensusId < consensusId){
            break;
        }
    }

    assert(false);

    return NULL;
}

static void Consensus_TriggerCallbacks(Consensus* consensus){
    Gears_listNode* instanceNode = NULL;
    if(!consensus->lastTrigger){
        instanceNode = Gears_listLast(consensus->consensusInstances);
    }else{
        instanceNode = Gears_listPrevNode(consensus->lastTrigger);
    }
    assert(instanceNode);
    ConsensusInstance* instance = Gears_listNodeValue(instanceNode);
    while(instance->consensusId == consensus->nextTriggeredId){
        if(!instance->learner.valueLeared){
            return;
        }
        void* additionalData = NULL;
        if(Consensus_ValEquals(instance->learner.val, instance->learner.originalVal)){
            additionalData = instance->additionalData;
        }
        instance->learner.callbackTriggered = true;
        consensus->approvedCallback(consensus->privateData, instance->learner.val + REDISMODULE_NODE_ID_LEN, instance->learner.len - REDISMODULE_NODE_ID_LEN, additionalData);

        consensus->lastTrigger = instanceNode;
        ++consensus->nextTriggeredId;

        instanceNode = Gears_listPrevNode(instanceNode);
        if(!instanceNode){
            return;
        }
        instance = Gears_listNodeValue(instanceNode);
    }
}

static void Consensus_LearnValueMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);
    size_t valLen = 0;
    const char* val = Gears_BufferReaderReadBuff(&br, &valLen);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    if(consnsusId <= consensus->minTriggered){
        // this is old and irelevent msg, ignore it!!
        return;
    }

    ConsensusInstance* instance = Consensus_InstanceGetOrCreate(consensus, consnsusId);

    if(instance->learner.proposalId > proposalId){
        return;
    }

    if(instance->learner.proposalId < proposalId){
        instance->learner.proposalId = proposalId;
        instance->learner.learnedNumber = 1;
        return;
    }

    ++instance->learner.learnedNumber;

    if(instance->learner.learnedNumber == (Cluster_GetSize() / 2) + 1){
        if(!instance->learner.valueLeared){

            instance->learner.val = Consensus_ValDup(val, valLen, &instance->learner.len);

            if(instance->learner.originalVal){
                if(!Consensus_ValEquals(instance->learner.originalVal, val)){
                    Consensus_Send(consensus, instance->learner.originalVal + REDISMODULE_NODE_ID_LEN, instance->learner.originalLen - REDISMODULE_NODE_ID_LEN, instance->additionalData);
                }
            }

            instance->learner.valueLeared = true;

            Consensus_TriggerCallbacks(consensus);
        }
    }
}

static void Consensus_ValueAcceptedMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    if(consnsusId <= consensus->minTriggered){
        // this is old and irelevent msg, ignore it!!
        return;
    }

    ConsensusInstance* instance = Consensus_InstanceGet(consensus, consnsusId);

    if(instance->phase != PHASE_TWO){
        // not in phase 2, just ignore it!!
        return;
    }

    if(instance->proposer.proposalId != proposalId){
        assert(instance->proposer.proposalId > proposalId);
        // this is an old reply, just ignore it!!
        return;
    }

    instance->proposer.acceptedNumber++;

    if(instance->proposer.acceptedNumber == (Cluster_GetSize() / 2) + 1){
        // we are done, we can rest now.
        instance->phase = PHASE_DONE;
    }
}

static void Consensus_AcceptDeniedMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    if(consnsusId <= consensus->minTriggered){
        // this is old and irelevent msg, ignore it!!
        return;
    }

    ConsensusInstance* instance = Consensus_InstanceGet(consensus, consnsusId);

    if(instance->phase != PHASE_TWO){
        // not in phase 2, just ignore it!!
        return;
    }

    if(instance->proposer.proposalId > proposalId){
        // this is an old deny reply, we can ignore it.
        return;
    }

    instance->proposer.proposalId = proposalId + 1;
    instance->proposer.acceptedNumber = 0;
    instance->proposer.recruitedNumber = 0;
    instance->proposer.biggerProposalId = 0;
    instance->phase = PHASE_ONE;

    Gears_Buffer *buf = Gears_BufferCreate();
    Gears_BufferWriter bw;
    Gears_BufferWriterInit(&bw, buf);
    Gears_BufferWriterWriteString(&bw, consensus->name);
    Gears_BufferWriterWriteLong(&bw, instance->consensusId);
    Gears_BufferWriterWriteLong(&bw, instance->proposer.proposalId);

    int r = (rand() % (GearsConfig_GetConsensusIdleEndInterval() + 1 - GearsConfig_GetConsensusIdleStartInterval())) + GearsConfig_GetConsensusIdleStartInterval();

    SendBuffCtx* sbctx = RG_ALLOC(sizeof(*sbctx));
    *sbctx = SendBuffCtx_Init(buf, ALL_AND_MYSENF, NULL, Consensus_RecruitMessage);

    Cluster_SendMsgToMySelfWithDelayM(Consensus_SendBuff, (char*)&sbctx, sizeof(sbctx), r);
}

static void Consensus_AcceptMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);
    size_t valLen = 0;
    const char* val = Gears_BufferReaderReadBuff(&br, &valLen);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    if(consnsusId <= consensus->minTriggered){
        // this is old and irelevent msg, ignore it!!
        return;
    }

    ConsensusInstance* instance = Consensus_InstanceGet(consensus, consnsusId);

    if(instance->acceptor.proposalId != proposalId){
        assert(instance->acceptor.proposalId > proposalId);
        Gears_Buffer *buf = Gears_BufferCreate();
        Gears_BufferWriter bw;
        Gears_BufferWriterInit(&bw, buf);
        Gears_BufferWriterWriteString(&bw, consensus->name);
        Gears_BufferWriterWriteLong(&bw, instance->consensusId);
        Gears_BufferWriterWriteLong(&bw, proposalId);

        Cluster_SendMsgM(sender_id, Consensus_AcceptDeniedMessage, buf->buff, buf->size);

        Gears_BufferFree(buf);
        return;
    }

    // accepting the value
    if(!Consensus_ValEquals(instance->acceptor.val, val)){
        if(instance->acceptor.val){
            RG_FREE(instance->acceptor.val);
        }
        instance->acceptor.val = Consensus_ValDup(val, valLen, &instance->acceptor.len);
    }

    Gears_Buffer *buf = Gears_BufferCreate();
    Gears_BufferWriter bw;
    Gears_BufferWriterInit(&bw, buf);
    Gears_BufferWriterWriteString(&bw, consensus->name);
    Gears_BufferWriterWriteLong(&bw, instance->consensusId);
    Gears_BufferWriterWriteLong(&bw, instance->acceptor.proposalId);
    Cluster_SendMsgM(sender_id, Consensus_ValueAcceptedMessage, buf->buff, buf->size);
    Gears_BufferWriterWriteBuff(&bw, instance->acceptor.val, instance->acceptor.len);
    Cluster_SendMsgToAllAndMyselfM(Consensus_LearnValueMessage, buf->buff, buf->size);

    Gears_BufferFree(buf);
}

static void Consensus_RecruitedMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);
    long long oldProposalId = Gears_BufferReaderReadLong(&br);
    long long hasValue = Gears_BufferReaderReadLong(&br);
    const char* val = NULL;
    size_t valLen = 0;
    if(hasValue){
        val = Gears_BufferReaderReadBuff(&br, &valLen);
    }

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    if(consnsusId <= consensus->minTriggered){
        // this is old and irelevent msg, ignore it!!
        return;
    }

    ConsensusInstance* instance = Consensus_InstanceGet(consensus, consnsusId);

    if(instance->phase != PHASE_ONE){
        // we are not in phase one anymore, we can ignore this deny message
        return;
    }

    if(instance->proposer.proposalId != proposalId){
        assert(instance->proposer.proposalId > proposalId);
        // this is an old reply, just ignore it!!
        return;
    }

    if(hasValue && instance->proposer.biggerProposalId < oldProposalId){

        if(!Consensus_ValEquals(instance->proposer.val, val)){
            if(instance->proposer.val){
                RG_FREE(instance->proposer.val);
            }
            instance->proposer.val = Consensus_ValDup(val, valLen, &instance->proposer.len);
        }
        instance->proposer.biggerProposalId = oldProposalId;

    }

    instance->proposer.recruitedNumber++;

    if(instance->proposer.recruitedNumber == (Cluster_GetSize() / 2) + 1){
        assert(sender_id); // not possible that we are the last one to accept ourself
        Gears_Buffer *buf = Gears_BufferCreate();
        Gears_BufferWriter bw;
        Gears_BufferWriterInit(&bw, buf);
        Gears_BufferWriterWriteString(&bw, consensus->name);
        Gears_BufferWriterWriteLong(&bw, instance->consensusId);
        Gears_BufferWriterWriteLong(&bw, instance->proposer.proposalId);
        Gears_BufferWriterWriteBuff(&bw, instance->proposer.val, instance->proposer.len);
        Cluster_SendMsgToAllAndMyselfM(Consensus_AcceptMessage, buf->buff, buf->size);

        Gears_BufferFree(buf);

        instance->phase = PHASE_TWO;
    }
}

static void Consensus_DeniedMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    // one deny tells us to restart
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    if(consnsusId <= consensus->minTriggered){
        // this is old and irelevent msg, ignore it!!
        return;
    }

    ConsensusInstance* instance = Consensus_InstanceGet(consensus, consnsusId);

    if(instance->phase != PHASE_ONE){
        // we are not in phase one anymore, we can ignore this deny message
        return;
    }

    if(instance->proposer.proposalId > proposalId){
        // this is an old deny reply, we can ignore it.
        return;
    }

    instance->proposer.proposalId = proposalId + 1;
    instance->proposer.acceptedNumber = 0;
    instance->proposer.recruitedNumber = 0;
    instance->proposer.biggerProposalId = 0;

    Gears_Buffer *buf = Gears_BufferCreate();
    Gears_BufferWriter bw;
    Gears_BufferWriterInit(&bw, buf);
    Gears_BufferWriterWriteString(&bw, consensus->name);
    Gears_BufferWriterWriteLong(&bw, instance->consensusId);
    Gears_BufferWriterWriteLong(&bw, instance->proposer.proposalId);

    int r = (rand() % (GearsConfig_GetConsensusIdleEndInterval() + 1 - GearsConfig_GetConsensusIdleStartInterval())) + GearsConfig_GetConsensusIdleStartInterval();

    SendBuffCtx* sbctx = RG_ALLOC(sizeof(*sbctx));
    *sbctx = SendBuffCtx_Init(buf, ALL_AND_MYSENF, NULL, Consensus_RecruitMessage);

    Cluster_SendMsgToMySelfWithDelayM(Consensus_SendBuff, (char*)&sbctx, sizeof(sbctx), r);
}

static void Consensus_RecruitMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    if(consnsusId <= consensus->minTriggered){
        // this is old and irelevent msg, ignore it!!
        return;
    }

    ConsensusInstance* instance = Consensus_InstanceGetOrCreate(consensus, consnsusId);

    bool recruited = false;
    long long oldPorposalId = instance->acceptor.proposalId;

    if(proposalId > instance->acceptor.proposalId){
        // Recruited, adobt the proposal id
        instance->acceptor.proposalId = proposalId;
        recruited = true;
    }
    Gears_Buffer *reply = Gears_BufferCreate();
    Gears_BufferWriter bw;
    Gears_BufferWriterInit(&bw, reply);
    Gears_BufferWriterWriteString(&bw, consensus->name);
    Gears_BufferWriterWriteLong(&bw, instance->consensusId);
    Gears_BufferWriterWriteLong(&bw, instance->acceptor.proposalId);
    Gears_BufferWriterWriteLong(&bw, oldPorposalId);

    if(recruited){
        if(instance->acceptor.val){
            Gears_BufferWriterWriteLong(&bw, 1); // has value
            Gears_BufferWriterWriteBuff(&bw, instance->acceptor.val, instance->acceptor.len);
        }else{
            Gears_BufferWriterWriteLong(&bw, 0); // no value
        }
        Cluster_SendMsgM(sender_id, Consensus_RecruitedMessage, reply->buff, reply->size);
    }else{
        Cluster_SendMsgM(sender_id, Consensus_DeniedMessage, reply->buff, reply->size);
    }

    Gears_BufferFree(reply);
}

typedef struct ConsensusMsgCtx{
    Consensus* consensus;
    char* msg;
    size_t len;
    void* additionalData;
}ConsensusMsgCtx;

static void Consensus_RecalculateMinConsensusTriggered(Consensus* consensus){
    if(Gears_dictSize(consensus->lastTriggeredDict) != Cluster_GetSize()){
        // we did not yet got information from all the nodes, there is not point calculating
        return;
    }
    Gears_dictIterator* iter = Gears_dictGetIterator(consensus->lastTriggeredDict);
    Gears_dictEntry *entry = NULL;
    long long min = INT64_MAX;
    while((entry = Gears_dictNext(iter))){
        long long id = Gears_dictGetUnsignedIntegerVal(entry);
        if (id < min){
            min = id;
        }
    }
    Gears_dictReleaseIterator(iter);

    consensus->minTriggered = min;
}

static void Consensus_LastIdTriggered(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long lastIdTriggered = Gears_BufferReaderReadLong(&br);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    Gears_dictEntry *exists;
    Gears_dictEntry *val = Gears_dictAddRaw(consensus->lastTriggeredDict, (char*)sender_id, &exists);
    if(val){
        val->v.u64 = lastIdTriggered;
        Consensus_RecalculateMinConsensusTriggered(consensus);
    }else{
        if(exists->v.u64 != lastIdTriggered){
            exists->v.u64 = lastIdTriggered;
            Consensus_RecalculateMinConsensusTriggered(consensus);
        }
    }
}

static void Consensus_CallbackTriggered(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    size_t msgLen = 0;
    const char* msg = Gears_BufferReaderReadBuff(&br, &msgLen);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    if(consnsusId <= consensus->minTriggered){
        // this is old and irelevent msg, ignore it!!
        return;
    }

    ConsensusInstance* instance = Consensus_InstanceGetOrCreate(consensus, consnsusId);

    if(!instance->learner.valueLeared){

        instance->learner.val = Consensus_ValDup(msg, msgLen, &instance->learner.len);

        if(instance->learner.originalVal){
            if(!Consensus_ValEquals(instance->learner.originalVal, msg)){
                Consensus_Send(consensus, instance->learner.originalVal + REDISMODULE_NODE_ID_LEN, instance->learner.originalLen - REDISMODULE_NODE_ID_LEN, instance->additionalData);
            }
        }

        instance->learner.valueLeared = true;

        Consensus_TriggerCallbacks(consensus);
    }
}

static void Consensus_LongPeriodicTasks(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer *buff = Gears_BufferCreate();
    Gears_BufferWriter bw;
    Gears_dictIterator *consensusIter = Gears_dictGetIterator(consensusDict);
    Gears_dictEntry *currEntry = NULL;
    while((currEntry = Gears_dictNext(consensusIter))){
        Consensus* consensus = Gears_dictGetVal(currEntry);
        if(consensus->lastTrigger){
            Gears_listNode* currNode = Gears_listLast(consensus->consensusInstances);
            Gears_listNode* nodeToStop = consensus->lastTrigger->prev;
            while(currNode != nodeToStop){
                ConsensusInstance* instance = Gears_listNodeValue(currNode);
                if(instance->consensusId > consensus->minTriggered){
                    Gears_BufferClear(buff);
                    Gears_BufferWriterInit(&bw, buff);
                    Gears_BufferWriterWriteString(&bw, consensus->name);
                    Gears_BufferWriterWriteLong(&bw, instance->consensusId);
                    Gears_BufferWriterWriteBuff(&bw, instance->learner.val, instance->learner.len);

                    Cluster_SendMsgUnreliableM(NULL, Consensus_CallbackTriggered, buff->buff, buff->size);
                }
                currNode = Gears_listPrevNode(currNode);
            }
        }
    }
    Gears_BufferFree(buff);
    Gears_dictReleaseIterator(consensusIter);
}

static void Consensus_ShortPeriodicTasks(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer *buff = Gears_BufferCreate();
    Gears_BufferWriter bw;
    Gears_dictIterator *consensusIter = Gears_dictGetIterator(consensusDict);
    Gears_dictEntry *currEntry = NULL;
    while((currEntry = Gears_dictNext(consensusIter))){
        Consensus* consensus = Gears_dictGetVal(currEntry);

        if(consensus->nextTriggeredId > 0){
            Gears_BufferClear(buff);
            Gears_BufferWriterInit(&bw, buff);
            Gears_BufferWriterWriteString(&bw, consensus->name);
            Gears_BufferWriterWriteLong(&bw, consensus->nextTriggeredId - 1);
            Cluster_SendMsgToAllAndMyselfUnreliableM(Consensus_LastIdTriggered, buff->buff, buff->size);
        }

        if(consensus->lastTrigger){
            Gears_listNode* currNode = Gears_listLast(consensus->consensusInstances);
            Gears_listNode* nodeToStop = consensus->lastTrigger->prev;
            while(currNode != nodeToStop){
                ConsensusInstance* instance = Gears_listNodeValue(currNode);
                if(instance->consensusId <= consensus->minTriggered){
                    if(consensus->appliedOnClusterCallback){
                        void* additionalData = NULL;
                        if(Consensus_ValEquals(instance->learner.val, instance->learner.originalVal)){
                            additionalData = instance->additionalData;
                        }
                        consensus->appliedOnClusterCallback(consensus->privateData, instance->learner.val + REDISMODULE_NODE_ID_LEN, instance->learner.len - REDISMODULE_NODE_ID_LEN, additionalData);
                    }
                    if(instance->proposer.val){
                        RG_FREE(instance->proposer.val);
                    }
                    if(instance->acceptor.val){
                        RG_FREE(instance->acceptor.val);
                    }
                    assert(instance->learner.val);
                    RG_FREE(instance->learner.val);
                    if(instance->learner.originalVal){
                        RG_FREE(instance->learner.originalVal);
                    }
                    RG_FREE(instance);
                    if(currNode == consensus->lastTrigger){
                        // we delete the consensus instance which consensus->pendingTrigger is
                        // pointing on, we should now point it to NULL cause the first consensus instance
                        // on our instances list is for sure not been triggered yet.
                        consensus->lastTrigger = NULL;
                    }
                    Gears_listNode* tempNode = currNode;
                    currNode = Gears_listPrevNode(currNode);
                    Gears_listDelNode(consensus->consensusInstances, tempNode);
                }else{
                    break;
                }
            }
        }
    }
    Gears_BufferFree(buff);
    Gears_dictReleaseIterator(consensusIter);
}

static void Consensus_StartInstance(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    ConsensusMsgCtx* cmctx = (*(ConsensusMsgCtx**)payload);

    ConsensusInstance* consensusInstance = Consensus_InstanceCreate(cmctx->consensus, -1);

    consensusInstance->proposer.val = (char*)cmctx->msg;
    consensusInstance->proposer.len = cmctx->len;
    consensusInstance->proposer.proposalId = 1; // we always start with proposal Id 1

    consensusInstance->additionalData = cmctx->additionalData;

    consensusInstance->learner.originalVal = Consensus_ValDup(cmctx->msg, cmctx->len, &consensusInstance->learner.originalLen);

    Gears_Buffer *buff = Gears_BufferCreate();
    Gears_BufferWriter bw;
    Gears_BufferWriterInit(&bw, buff);
    Gears_BufferWriterWriteString(&bw, cmctx->consensus->name);
    Gears_BufferWriterWriteLong(&bw, consensusInstance->consensusId);
    Gears_BufferWriterWriteLong(&bw, consensusInstance->proposer.proposalId);

    Cluster_SendMsgToAllAndMyselfM(Consensus_RecruitMessage, buff->buff, buff->size);

    RG_FREE(cmctx);

    Gears_BufferFree(buff);
}

char* TestConsensusVal = NULL;

static void Consensus_TestOnMsgAproved(void* privateData, const char* msg, size_t len, void* additionalData){
    RedisModuleCtx* ctx = RedisModule_GetThreadSafeContext(NULL);
    LockHandler_Acquire(ctx);
    if(TestConsensusVal){
        RG_FREE(TestConsensusVal);
    }
    TestConsensusVal = RG_STRDUP(msg);
    LockHandler_Release(ctx);
    RedisModule_FreeThreadSafeContext(ctx);
}

static void Consensus_ReplyInfo(RedisModuleCtx *unused, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    RedisModuleBlockedClient* bc = (*(RedisModuleBlockedClient**)payload);
    RedisModuleCtx* ctx = RedisModule_GetThreadSafeContext(bc);
    RedisModule_ThreadSafeContextLock(ctx);
    RedisModule_ReplyWithArray(ctx, Gears_dictSize(consensusDict));
    Gears_dictIterator *iter = Gears_dictGetIterator(consensusDict);
    Gears_dictEntry *entry = NULL;
    while((entry = Gears_dictNext(iter))){
        RedisModule_ReplyWithArray(ctx, 8);
        Consensus* consensus = Gears_dictGetVal(entry);
        RedisModule_ReplyWithStringBuffer(ctx, "name", strlen("name"));
        RedisModule_ReplyWithStringBuffer(ctx, consensus->name, strlen(consensus->name));
        RedisModule_ReplyWithStringBuffer(ctx, "MinTriggered", strlen("MinTriggered"));
        RedisModule_ReplyWithLongLong(ctx, consensus->minTriggered);
        RedisModule_ReplyWithStringBuffer(ctx, "NextTrigger", strlen("NextTrigger"));
        RedisModule_ReplyWithLongLong(ctx, consensus->nextTriggeredId);
        RedisModule_ReplyWithStringBuffer(ctx, "ConsensusInstances", strlen("ConsensusInstances"));
        RedisModule_ReplyWithArray(ctx, Gears_listLength(consensus->consensusInstances));
        Gears_listIter *listIter = Gears_listGetIterator(consensus->consensusInstances, AL_START_HEAD);
        Gears_listNode *node = NULL;
        while((node = Gears_listNext(listIter))){
            ConsensusInstance* instance = Gears_listNodeValue(node);
            RedisModule_ReplyWithArray(ctx, 10);
            RedisModule_ReplyWithStringBuffer(ctx, "ConsensusId", strlen("ConsensusId"));
            RedisModule_ReplyWithLongLong(ctx, instance->consensusId);
            RedisModule_ReplyWithStringBuffer(ctx, "Phase", strlen("Phase"));
            RedisModule_ReplyWithLongLong(ctx, instance->phase);
            RedisModule_ReplyWithStringBuffer(ctx, "IsValueLearned", strlen("IsValueLearned"));
            RedisModule_ReplyWithLongLong(ctx, instance->learner.valueLeared);
            RedisModule_ReplyWithStringBuffer(ctx, "LearnedValue", strlen("LearnedValue"));
            RedisModule_ReplyWithStringBuffer(ctx, instance->learner.val, instance->learner.len);
            RedisModule_ReplyWithStringBuffer(ctx, "CallbackTriggered", strlen("CallbackTriggered"));
            RedisModule_ReplyWithLongLong(ctx, instance->learner.callbackTriggered);
        }
        Gears_listReleaseIterator(listIter);
    }
    Gears_dictReleaseIterator(iter);
    RedisModule_UnblockClient(bc, NULL);
    RedisModule_ThreadSafeContextUnlock(ctx);
    RedisModule_FreeThreadSafeContext(ctx);
}

static int Consensus_Info(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 100000);
    Cluster_SendMsgToMySelfM(Consensus_ReplyInfo, (char*)&bc, sizeof(RedisModuleBlockedClient*));
    return REDISMODULE_OK;
}

static int Consensus_TestGet(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    if(TestConsensusVal){
        RedisModule_ReplyWithStringBuffer(ctx, TestConsensusVal, strlen(TestConsensusVal));
    }else{
        RedisModule_ReplyWithNull(ctx);
    }
    return REDISMODULE_OK;
}

static int Consensus_TestSet(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    if(argc != 2){
        return RedisModule_WrongArity(ctx);
    }

    size_t len;
    const char* msg = RedisModule_StringPtrLen(argv[1], &len);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, "TestConsensus");

    Consensus_Send(consensus, msg, len + 1, NULL);

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int Consensus_Init(RedisModuleCtx* ctx){
    unsigned int seed;
    RedisModule_GetRandomHexChars((char *)&seed, sizeof(unsigned int));
    srand(seed);

    consensusDict = Gears_dictCreate(&Gears_dictTypeHeapStrings, NULL);
    Cluster_RegisterMsgReceiverM(Consensus_SendBuff);
    Cluster_RegisterMsgReceiverM(Consensus_StartInstance);
    Cluster_RegisterMsgReceiverM(Consensus_RecruitMessage);
    Cluster_RegisterMsgReceiverM(Consensus_RecruitedMessage);
    Cluster_RegisterMsgReceiverM(Consensus_DeniedMessage);
    Cluster_RegisterMsgReceiverM(Consensus_AcceptMessage);
    Cluster_RegisterMsgReceiverM(Consensus_AcceptDeniedMessage);
    Cluster_RegisterMsgReceiverM(Consensus_ValueAcceptedMessage);
    Cluster_RegisterMsgReceiverM(Consensus_LearnValueMessage);
    Cluster_RegisterMsgReceiverM(Consensus_ShortPeriodicTasks);
    Cluster_RegisterMsgReceiverM(Consensus_LongPeriodicTasks);
    Cluster_RegisterMsgReceiverM(Consensus_CallbackTriggered);
    Cluster_RegisterMsgReceiverM(Consensus_LastIdTriggered);
    Cluster_RegisterMsgReceiverM(Consensus_ReplyInfo);

    Consensus_Create("TestConsensus", Consensus_TestOnMsgAproved, NULL, NULL);

    if (RedisModule_CreateCommand(ctx, "rg.testconsensusset", Consensus_TestSet, "readonly", 0, 0, 0) != REDISMODULE_OK){
        RedisModule_Log(ctx, "warning", "could not register command rg.testconsensusset");
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "rg.testconsensusget", Consensus_TestGet, "readonly", 0, 0, 0) != REDISMODULE_OK){
        RedisModule_Log(ctx, "warning", "could not register command rg.testconsensusget");
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "rg.infoconsensus", Consensus_Info, "readonly", 0, 0, 0) != REDISMODULE_OK){
        RedisModule_Log(ctx, "warning", "could not register command rg.infoconsensus");
        return REDISMODULE_ERR;
    }

    Cluster_SendPersisMsgToMySelfM(Consensus_ShortPeriodicTasks, NULL, 0, GearsConfig_GetConsensusShortPeriodicTasksInterval());
    Cluster_SendPersisMsgToMySelfM(Consensus_LongPeriodicTasks, NULL, 0, GearsConfig_GetConsensusLongPeriodicTasksInterval());

    return REDISMODULE_OK;
}

Consensus* Consensus_Create(const char* name, Consensus_OnMsgAproved approvedCallback,
                            Consensus_OnMsgAppliedOnCluster appliedOnClusterCallback,
                            void* privateData){
    Consensus* consensus = RG_ALLOC(sizeof(*consensus));
    consensus->name = RG_STRDUP(name);
    consensus->approvedCallback = approvedCallback;
    consensus->appliedOnClusterCallback = appliedOnClusterCallback;
    consensus->privateData = privateData;
    consensus->currConsensusId = 0;
    consensus->lastTrigger = NULL;
    consensus->nextTriggeredId = 0;
    consensus->consensusInstances = Gears_listCreate();
    consensus->lastTriggeredDict = Gears_dictCreate(&Gears_dictTypeHeapStrings, NULL);
    consensus->minTriggered = -1;

    Gears_dictAdd(consensusDict, consensus->name, consensus);

    return consensus;
}

void Consensus_Send(Consensus* consensus, const char* msg, size_t len, void* additionalData){
    if(!Cluster_IsClusterMode()){
        consensus->approvedCallback(consensus->privateData, msg, len, additionalData);
        return;
    }

    ConsensusMsgCtx* cmctx = RG_ALLOC(sizeof(ConsensusMsgCtx));
    cmctx->consensus = consensus;
    cmctx->msg = RG_ALLOC(REDISMODULE_NODE_ID_LEN + len);
    // we are adding the node_id to the message so messages from different nodes
    // with the same value will be different.
    memcpy(cmctx->msg, Cluster_GetMyId(), REDISMODULE_NODE_ID_LEN);
    memcpy(cmctx->msg + REDISMODULE_NODE_ID_LEN, msg, len);
    cmctx->len = REDISMODULE_NODE_ID_LEN + len;
    cmctx->additionalData = additionalData;
    Cluster_SendMsgToMySelfM(Consensus_StartInstance, (char*)&cmctx, sizeof(ConsensusMsgCtx*));
}




