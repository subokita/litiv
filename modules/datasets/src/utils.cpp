
// This file is part of the LITIV framework; visit the original repository at
// https://github.com/plstcharles/litiv for more information.
//
// Copyright 2015 Pierre-Luc St-Charles; pierre-luc.st-charles<at>polymtl.ca
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <litiv/datasets/utils.hpp>
#include "litiv/datasets/utils.hpp"

#define HARDCODE_IMAGE_PACKET_INDEX        0 // for sync debug only! will corrupt data for non-image packets
#define CONSOLE_DEBUG                      0
#define PRECACHE_REQUEST_TIMEOUT_MS        1
#define PRECACHE_QUERY_TIMEOUT_MS          10
#define PRECACHE_QUERY_END_TIMEOUT_MS      500
#define PRECACHE_REFILL_TIMEOUT_MS         10000
#if (!(defined(_M_X64) || defined(__amd64__)) && CACHE_MAX_SIZE_GB>2)
#error "Cache max size exceeds system limit (x86)."
#endif //(!(defined(_M_X64) || defined(__amd64__)) && CACHE_MAX_SIZE_GB>2)
#define CACHE_MAX_SIZE size_t(((CACHE_MAX_SIZE_GB*1024)*1024)*1024)
#define CACHE_MIN_SIZE size_t(((10)*1024)*1024) // 10mb

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

std::string lv::IDataHandler::getInputName(size_t nPacketIdx) const {
    std::array<char,32> acBuffer;
    snprintf(acBuffer.data(),acBuffer.size(),getInputCount()<1e7?"%06zu":"%09zu",nPacketIdx);
    return std::string(acBuffer.data());
}

std::string lv::IDataHandler::getOutputName(size_t nPacketIdx) const {
    const auto pProducer = shared_from_this_cast<const IDataProducer_<DatasetSource_Image>>();
    if(pProducer && pProducer->getIOMappingType()<=IndexMapping)
        return getInputName(nPacketIdx); // will reuse input image file name as output name
    std::array<char,32> acBuffer;
    snprintf(acBuffer.data(),acBuffer.size(),getExpectedOutputCount()<1e7?"%06zu":"%09zu",nPacketIdx);
    return std::string(acBuffer.data());
}

bool lv::IDataHandler::compare(const IDataHandler* i, const IDataHandler* j) {
    return lv::compare_lowercase(i->getName(),j->getName());
}

bool lv::IDataHandler::compare_load(const IDataHandler* i, const IDataHandler* j) {
    return i->getExpectedLoad()<j->getExpectedLoad();
}

bool lv::IDataHandler::compare(const IDataHandler& i, const IDataHandler& j) {
    return lv::compare_lowercase(i.getName(),j.getName());
}

bool lv::IDataHandler::compare_load(const IDataHandler& i, const IDataHandler& j) {
    return i.getExpectedLoad()<j.getExpectedLoad();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

bool lv::IGroupDataParser::isBare() const {
    return m_bIsBare;
}

bool lv::IGroupDataParser::isGroup() const {
    return true;
}

lv::IDataHandlerPtrArray lv::IGroupDataParser::getBatches(bool bWithHierarchy) const {
    if(bWithHierarchy)
        return m_vpBatches;
    IDataHandlerPtrArray vpBatches;
    std::function<void(const IDataHandlerPtr&)> lPushBatches = [&](const IDataHandlerPtr& pBatch) {
        if(pBatch->isGroup())
            for(const auto& pSubBatch : pBatch->getBatches(true))
                lPushBatches(pSubBatch);
        else
            vpBatches.push_back(pBatch);
    };
    for(const auto& pBatch : getBatches(true))
        lPushBatches(pBatch);
    return vpBatches;
}

void lv::IGroupDataParser::startPrecaching(bool bPrecacheGT, size_t nSuggestedBufferSize) {
    for(const auto& pBatch : getBatches(true))
        pBatch->startPrecaching(bPrecacheGT,nSuggestedBufferSize);
}

void lv::IGroupDataParser::stopPrecaching() {
    for(const auto& pBatch : getBatches(true))
        pBatch->stopPrecaching();
}

bool lv::IGroupDataParser::isProcessing() const {
    for(const auto& pBatch : getBatches(true))
        if(pBatch->isProcessing())
            return true;
    return false;
}

double lv::IGroupDataParser::getProcessTime() const {
    return lv::accumulateMembers<double,IDataHandlerPtr>(getBatches(true),[](const IDataHandlerPtr& p){return p->getProcessTime();});
}

double lv::IGroupDataParser::getExpectedLoad() const {
    return lv::accumulateMembers<double,IDataHandlerPtr>(getBatches(true),[](const IDataHandlerPtr& p){return p->getExpectedLoad();});
}

size_t lv::IGroupDataParser::getInputCount() const {
    return lv::accumulateMembers<size_t,IDataHandlerPtr>(getBatches(true),[](const IDataHandlerPtr& p){return p->getInputCount();});
}

size_t lv::IGroupDataParser::getGTCount() const {
    return lv::accumulateMembers<size_t,IDataHandlerPtr>(getBatches(true),[](const IDataHandlerPtr& p){return p->getGTCount();});
}

void lv::IGroupDataParser::parseData() {
    m_vpBatches.clear();
    m_bIsBare = true;
    IDatasetPtr pDataset = getDatasetInfo();
    const std::string& sRelativePath = getRelativePath();
    if(!lv::string_contains_token(getName(),pDataset->getSkippedDirTokens())) {
        std::cout << "\tParsing directory '" << pDataset->getDatasetPath()+sRelativePath << "' for work group '" << getName() << "'..." << std::endl;
        std::vector<std::string> vsWorkBatchPaths;
        // by default, all subdirs are considered work batch directories (if none, the category directory itself is a batch, and 'bare')
        lv::GetSubDirsFromDir(getDataPath(),vsWorkBatchPaths);
        if(vsWorkBatchPaths.empty())
            m_vpBatches.push_back(createWorkBatch(getName(),getRelativePath()));
        else {
            m_bIsBare = false;
            for(const auto& sPathIter : vsWorkBatchPaths) {
                const size_t nLastSlashPos = sPathIter.find_last_of("/\\");
                const std::string sNewBatchName = nLastSlashPos==std::string::npos?sPathIter:sPathIter.substr(nLastSlashPos+1);
                if(!lv::string_contains_token(sNewBatchName,pDataset->getSkippedDirTokens()))
                    m_vpBatches.push_back(createWorkBatch(sNewBatchName,lv::AddDirSlashIfMissing(getRelativePath())+sNewBatchName+"/"));
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

lv::DataPrecacher::DataPrecacher(std::function<const cv::Mat&(size_t)> lDataLoaderCallback) :
        m_lCallback(lDataLoaderCallback) {
    lvAssert_(m_lCallback,"invalid data precacher callback");
    m_bIsActive = false;
    m_nAnswIdx = m_nReqIdx = m_nLastReqIdx = size_t(-1);
}

lv::DataPrecacher::~DataPrecacher() {
    stopAsyncPrecaching();
}

const cv::Mat& lv::DataPrecacher::getPacket(size_t nIdx) {
    if(nIdx==m_nLastReqIdx)
        return m_oLastReqPacket;
    else if(!m_bIsActive) {
        m_oLastReqPacket = m_lCallback(nIdx);
        m_nLastReqIdx = nIdx;
        return m_oLastReqPacket;
    }
    std::mutex_unique_lock sync_lock(m_oSyncMutex);
    m_nReqIdx = nIdx;
    std::cv_status res;
    size_t nAnswIdx;
    do {
        m_oReqCondVar.notify_one();
        res = m_oSyncCondVar.wait_for(sync_lock,std::chrono::milliseconds(PRECACHE_REQUEST_TIMEOUT_MS));
        nAnswIdx = m_nAnswIdx.load();
#if CONSOLE_DEBUG
        if(res==std::cv_status::timeout && nAnswIdx!=m_nReqIdx)
            std::cout << "data precacher [" << uintptr_t(this) << "] retrying request for packet #" << nIdx << "..." << std::endl;
#endif //CONSOLE_DEBUG
    } while(res==std::cv_status::timeout && nAnswIdx!=m_nReqIdx);
    m_oLastReqPacket = m_oReqPacket;
    m_nLastReqIdx = nAnswIdx;
    return m_oLastReqPacket;
}

bool lv::DataPrecacher::startAsyncPrecaching(size_t nSuggestedBufferSize) {
    static_assert(PRECACHE_REQUEST_TIMEOUT_MS>0,"Precache request timeout must be a positive value");
    static_assert(PRECACHE_QUERY_TIMEOUT_MS>0,"Precache query timeout must be a positive value");
    static_assert(PRECACHE_QUERY_END_TIMEOUT_MS>0,"Precache query post-end timeout must be a positive value");
    static_assert(PRECACHE_REFILL_TIMEOUT_MS>0,"Precache refill timeout must be a positive value");
    if(m_bIsActive)
        stopAsyncPrecaching();
    if(nSuggestedBufferSize>0) {
        m_bIsActive = true;
        m_nAnswIdx = m_nReqIdx = size_t(-1);
        m_hWorker = std::thread(&DataPrecacher::entry,this,std::max(std::min(nSuggestedBufferSize,CACHE_MAX_SIZE),CACHE_MIN_SIZE));
    }
    return m_bIsActive;
}

void lv::DataPrecacher::stopAsyncPrecaching() {
    if(m_bIsActive) {
        m_bIsActive = false;
        m_hWorker.join();
    }
}

void lv::DataPrecacher::entry(const size_t nBufferSize) {
    std::mutex_unique_lock sync_lock(m_oSyncMutex);
#if CONSOLE_DEBUG
    std::cout << "data precacher [" << uintptr_t(this) << "] init w/ buffer size = " << (nBufferSize/1024)/1024 << " mb" << std::endl;
#endif //CONSOLE_DEBUG
    std::queue<cv::Mat> qoCache;
    std::vector<uchar> vcBuffer(nBufferSize);
    size_t nNextExpectedReqIdx = 0;
    size_t nNextPrecacheIdx = 0;
    size_t nFirstBufferIdx = size_t(-1);
    size_t nNextBufferIdx = size_t(-1);
    bool bReachedEnd = false;
    const auto lCacheNextPacket = [&]() -> size_t {
        const cv::Mat& oNextPacket = m_lCallback(nNextPrecacheIdx);
        const size_t nNextPacketSize = oNextPacket.total()*oNextPacket.elemSize();
        if(nNextPacketSize==0) {
            bReachedEnd = true;
            return 0;
        }
        else if(nFirstBufferIdx<=nNextBufferIdx) {
            bReachedEnd = false;
            if(nNextBufferIdx==size_t(-1) || (nNextBufferIdx+nNextPacketSize>=nBufferSize)) {
                if((nFirstBufferIdx!=size_t(-1) && nNextPacketSize>=nFirstBufferIdx) || nNextPacketSize>=nBufferSize)
                    return 0;
                cv::Mat oNextPacket_cache(oNextPacket.size(),oNextPacket.type(),vcBuffer.data());
                oNextPacket.copyTo(oNextPacket_cache);
                qoCache.push(oNextPacket_cache);
                nNextBufferIdx = nNextPacketSize;
                if(nFirstBufferIdx==size_t(-1))
                    nFirstBufferIdx = 0;
            }
            else { // nNextBufferIdx+nNextPacketSize<m_nBufferSize
                cv::Mat oNextPacket_cache(oNextPacket.size(),oNextPacket.type(),vcBuffer.data()+nNextBufferIdx);
                oNextPacket.copyTo(oNextPacket_cache);
                qoCache.push(oNextPacket_cache);
                nNextBufferIdx += nNextPacketSize;
            }
        }
        else if(nNextBufferIdx+nNextPacketSize<nFirstBufferIdx) {
            cv::Mat oNextPacket_cache(oNextPacket.size(),oNextPacket.type(),vcBuffer.data()+nNextBufferIdx);
            oNextPacket.copyTo(oNextPacket_cache);
            qoCache.push(oNextPacket_cache);
            nNextBufferIdx += nNextPacketSize;
        }
        else // nNextBufferIdx+nNextPacketSize>=nFirstBufferIdx
            return 0;
        ++nNextPrecacheIdx;
#if CONSOLE_DEBUG
        //std::cout << "data precacher [" << uintptr_t(this) << "] filled one packet w/ size = " << nNextPacketSize/1024 << " kb" << std::endl;
#endif //CONSOLE_DEBUG
        return nNextPacketSize;
    };
    const std::chrono::time_point<std::chrono::high_resolution_clock> nPrefillTick = std::chrono::high_resolution_clock::now();
    while(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now()-nPrefillTick).count()<PRECACHE_REFILL_TIMEOUT_MS && lCacheNextPacket());
    while(m_bIsActive) {
        if(m_oReqCondVar.wait_for(sync_lock,std::chrono::milliseconds(bReachedEnd?PRECACHE_QUERY_END_TIMEOUT_MS:PRECACHE_QUERY_TIMEOUT_MS))!=std::cv_status::timeout) {
            if(m_nReqIdx!=nNextExpectedReqIdx-1) {
                if(!qoCache.empty()) {
                    if(m_nReqIdx<nNextPrecacheIdx && m_nReqIdx>=nNextExpectedReqIdx) {
#if CONSOLE_DEBUG
                        if(m_nReqIdx>nNextExpectedReqIdx)
                            std::cout << "data precacher [" << uintptr_t(this) << "] popping " << m_nReqIdx-nNextExpectedReqIdx << " extra packet(s) from cache" << std::endl;
#endif //CONSOLE_DEBUG
                        while(m_nReqIdx-nNextExpectedReqIdx+1>0) {
                            m_oReqPacket = qoCache.front();
                            m_nAnswIdx = m_nReqIdx;
                            nFirstBufferIdx = (size_t)(m_oReqPacket.data-vcBuffer.data());
                            qoCache.pop();
                            ++nNextExpectedReqIdx;
                        }
                    }
                    else {
#if CONSOLE_DEBUG
                        std::cout << "data precacher [" << uintptr_t(this) << "] out-of-order request, destroying cache" << std::endl;
#endif //CONSOLE_DEBUG
                        qoCache = std::queue<cv::Mat>();
                        m_oReqPacket = m_lCallback(m_nReqIdx);
                        m_nAnswIdx = m_nReqIdx;
                        nFirstBufferIdx = nNextBufferIdx = size_t(-1);
                        nNextExpectedReqIdx = nNextPrecacheIdx = m_nReqIdx+1;
                        bReachedEnd = false;
                    }
                }
                else {
#if CONSOLE_DEBUG
                    std::cout << "data precacher [" << uintptr_t(this) << "] answering request manually, precaching is falling behind" << std::endl;
#endif //CONSOLE_DEBUG
                    m_oReqPacket = m_lCallback(m_nReqIdx);
                    m_nAnswIdx = m_nReqIdx;
                    nFirstBufferIdx = nNextBufferIdx = size_t(-1);
                    nNextExpectedReqIdx = nNextPrecacheIdx = m_nReqIdx+1;
                }
            }
#if CONSOLE_DEBUG
            else
                std::cout << "data precacher [" << uintptr_t(this) << "] answering request using last packet" << std::endl;
#endif //CONSOLE_DEBUG
            m_oSyncCondVar.notify_one();
            lCacheNextPacket();
        }
        else if(!bReachedEnd) {
            const size_t nUsedBufferSize = nFirstBufferIdx==size_t(-1)?0:(nFirstBufferIdx<nNextBufferIdx?nNextBufferIdx-nFirstBufferIdx:nBufferSize-nFirstBufferIdx+nNextBufferIdx);
            if(nUsedBufferSize<nBufferSize/4) {
#if CONSOLE_DEBUG
                std::cout << "data precacher [" << uintptr_t(this) << "] force refilling precache buffer... (current size = " << (nUsedBufferSize/1024)/1024 << " mb)" << std::endl;
#endif //CONSOLE_DEBUG
                size_t nFillCount = 0;
                const std::chrono::time_point<std::chrono::high_resolution_clock> nRefillTick = std::chrono::high_resolution_clock::now();
                while(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now()-nRefillTick).count()<PRECACHE_REFILL_TIMEOUT_MS && nFillCount++<10 && lCacheNextPacket());
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

void lv::IIDataLoader::startPrecaching(bool bPrecacheGT, size_t nSuggestedBufferSize) {
    lvAssert_(m_oInputPrecacher.startAsyncPrecaching(nSuggestedBufferSize),"could not start precaching input packets");
    lvAssert_(!bPrecacheGT || m_oGTPrecacher.startAsyncPrecaching(nSuggestedBufferSize),"could not start precaching gt packets");
}

void lv::IIDataLoader::stopPrecaching() {
    m_oInputPrecacher.stopAsyncPrecaching();
    m_oGTPrecacher.stopAsyncPrecaching();
}

const cv::Mat& lv::IIDataLoader::getInput(size_t nPacketIdx) {
    return m_oInputPrecacher.getPacket(nPacketIdx);
}

const cv::Mat& lv::IIDataLoader::getGT(size_t nPacketIdx) {
    return m_oGTPrecacher.getPacket(nPacketIdx);
}

const cv::Mat& lv::IIDataLoader::getInputROI(size_t /*nPacketIdx*/) const {
    return cv::emptyMat();
}

const cv::Mat& lv::IIDataLoader::getGTROI(size_t /*nPacketIdx*/) const {
    return cv::emptyMat();
}

const cv::Size& lv::IIDataLoader::getInputSize(size_t /*nPacketIdx*/) const {
    return cv::emptySize();
}

const cv::Size& lv::IIDataLoader::getGTSize(size_t /*nPacketIdx*/) const {
    return cv::emptySize();
}

const cv::Size& lv::IIDataLoader::getInputMaxSize() const {
    return cv::emptySize();
}

const cv::Size& lv::IIDataLoader::getGTMaxSize() const {
    return cv::emptySize();
}

lv::IIDataLoader::IIDataLoader(PacketPolicy eInputType, PacketPolicy eGTType, PacketPolicy eOutputType, MappingPolicy eGTMappingType, MappingPolicy eIOMappingType) :
        m_oInputPrecacher(std::bind(&IIDataLoader::getInput_redirect,this,std::placeholders::_1)),
        m_oGTPrecacher(std::bind(&IIDataLoader::getGT_redirect,this,std::placeholders::_1)),
        m_eInputType(eInputType),m_eGTType(eGTType),m_eOutputType(eOutputType),m_eGTMappingType(eGTMappingType),m_eIOMappingType(eIOMappingType) {}

const cv::Mat& lv::IIDataLoader::getInput_redirect(size_t nIdx) {
    m_oLatestInput = getRawInput(nIdx);
    if(!m_oLatestInput.empty()) {
        if(m_eInputType==ImagePacket) {
#if HARDCODE_IMAGE_PACKET_INDEX
            std::stringstream sstr;
            sstr << "Packet #" << nIdx;
            writeOnImage(m_oLatestInput,sstr.str(),cv::Scalar_<uchar>::all(255);
#endif //HARDCODE_IMAGE_PACKET_INDEX
            if(getDatasetInfo()->is4ByteAligned() && m_oLatestInput.channels()==3)
                cv::cvtColor(m_oLatestInput,m_oLatestInput,cv::COLOR_BGR2BGRA);
            const cv::Size& oPacketSize = getInputSize(nIdx);
            if(oPacketSize.area()>0 && m_oLatestInput.size()!=oPacketSize)
                cv::resize(m_oLatestInput,m_oLatestInput,oPacketSize,0,0,cv::INTER_NEAREST);
        }
    }
    return m_oLatestInput;
}

const cv::Mat& lv::IIDataLoader::getGT_redirect(size_t nIdx) {
    m_oLatestGT = getRawGT(nIdx);
    if(!m_oLatestGT.empty()) {
        if(m_eGTType==ImagePacket) {
#if HARDCODE_IMAGE_PACKET_INDEX
            std::stringstream sstr;
            sstr << "Packet #" << nIdx;
            writeOnImage(m_oLatestGT,sstr.str(),cv::Scalar_<uchar>::all(255);
#endif //HARDCODE_IMAGE_PACKET_INDEX
            if(getDatasetInfo()->is4ByteAligned() && m_oLatestGT.channels()==3)
                cv::cvtColor(m_oLatestGT,m_oLatestGT,cv::COLOR_BGR2BGRA);
            const cv::Size& oPacketSize = getGTSize(nIdx);
            if(oPacketSize.area()>0 && m_oLatestGT.size()!=oPacketSize)
                cv::resize(m_oLatestGT,m_oLatestGT,oPacketSize,0,0,cv::INTER_NEAREST);
        }
    }
    return m_oLatestGT;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

size_t lv::IDataLoader_<lv::Array>::getInputStreamCount() const {
    return 1;
}

size_t lv::IDataLoader_<lv::Array>::getGTStreamCount() const {
    return 1;
}

std::string lv::IDataLoader_<lv::Array>::getInputStreamName(size_t nStreamIdx) const {
    return cv::format("in[%02d]",(int)nStreamIdx);
}

std::string lv::IDataLoader_<lv::Array>::getGTStreamName(size_t nStreamIdx) const {
    return cv::format("gt[%02d]",(int)nStreamIdx);
}

const std::vector<cv::Mat>& lv::IDataLoader_<lv::Array>::getInputArray(size_t nPacketIdx) {
    if(getInputStreamCount()==0)
        return cv::emptyMatArray();
    // add last check logic...?
    m_vLatestUnpackedInput.resize(getInputStreamCount());
    unpackInput(nPacketIdx,m_vLatestUnpackedInput);
    return m_vLatestUnpackedInput;
}

const std::vector<cv::Mat>& lv::IDataLoader_<lv::Array>::getGTArray(size_t nPacketIdx) {
    if(getGTStreamCount()==0)
        return cv::emptyMatArray();
    // add last check logic...?
    m_vLatestUnpackedGT.resize(getGTStreamCount());
    unpackGT(nPacketIdx,m_vLatestUnpackedGT);
    return m_vLatestUnpackedGT;
}

const std::vector<cv::Mat>& lv::IDataLoader_<lv::Array>::getInputROIArray(size_t /*nPacketIdx*/) const {
    return cv::emptyMatArray();
}

const std::vector<cv::Mat>& lv::IDataLoader_<lv::Array>::getGTROIArray(size_t /*nPacketIdx*/) const {
    return cv::emptyMatArray();
}

const std::vector<cv::Size>& lv::IDataLoader_<lv::Array>::getInputSizeArray(size_t /*nPacketIdx*/) const {
    return cv::emptySizeArray();
}

const std::vector<cv::Size>& lv::IDataLoader_<lv::Array>::getGTSizeArray(size_t /*nPacketIdx*/) const {
    return cv::emptySizeArray();
}

bool lv::IDataLoader_<lv::Array>::isStreamGrayscale(size_t /*nStreamIdx*/) const {
    return isGrayscale();
}

void lv::IDataLoader_<lv::Array>::unpackInput(size_t nPacketIdx, std::vector<cv::Mat>& vUnpackedInput) {
    // no need to clone if getInput does not allow reentrancy --- output mats in the vector will stay valid for as long as oInput is valid (typically until next getInput call)
    const cv::Mat& oInput = getInput(nPacketIdx)/*.clone()*/;
    if(getInputPacketType()==ImagePacket)
        vUnpackedInput[0] = oInput;
    else if(getInputPacketType()==ImageArrayPacket) {
        const std::vector<cv::Size>& vSizes = getInputSizeArray(nPacketIdx);
        if(vSizes.empty() || vSizes.size()!=vUnpackedInput.size())
            lvError("cannot handle image array packet type in unpackInput due to missing packet size(s)");
        if(oInput.empty()) {
            for(size_t s=0; s<vSizes.size(); ++s)
                vUnpackedInput[s] = cv::Mat();
        }
        else {
            lvAssert(oInput.isContinuous());
            // for now, we assume all stream packets have the type of the original packed matrix
            uintptr_t nCurrPacketIdx = (uintptr_t)oInput.datastart;
            for(size_t s=0; s<vSizes.size(); ++s) {
                const size_t nCurrPacketSize = oInput.elemSize()*vSizes[s].area();
                lvAssert_(nCurrPacketIdx+nCurrPacketSize<uintptr_t(oInput.dataend),"unpack out-of-bounds");
                if(nCurrPacketSize>0)
                    vUnpackedInput[s] = cv::Mat(vSizes[s],oInput.type(),(void*)nCurrPacketIdx);
                else
                    vUnpackedInput[s] = cv::Mat();
                nCurrPacketIdx += nCurrPacketSize;
            }
            lvAssert_(nCurrPacketIdx==uintptr_t(oInput.dataend),"unpack has leftover data");
        }
    }
    else
        lvError("unhandled packet type in unpackInput");
}

void lv::IDataLoader_<lv::Array>::unpackGT(size_t nPacketIdx, std::vector<cv::Mat>& vUnpackedGT) {
    // no need to clone if getGT does not allow reentrancy --- output mats in the vector will stay valid for as long as oGT is valid (typically until next getGT call)
    const cv::Mat& oGT = getGT(nPacketIdx)/*.clone()*/;
    if(getGTPacketType()==ImagePacket)
        vUnpackedGT[0] = oGT;
    else if(getGTPacketType()==ImageArrayPacket) {
        const std::vector<cv::Size>& vSizes = getGTSizeArray(nPacketIdx);
        if(vSizes.empty() || vSizes.size()!=vUnpackedGT.size())
            lvError("cannot handle image array packet type in unpackGT due to missing packet size(s)");
        if(oGT.empty()) {
            for(size_t s=0; s<vSizes.size(); ++s)
                vUnpackedGT[s] = cv::Mat();
        }
        else {
            lvAssert(oGT.isContinuous());
            // for now, we assume all stream packets have the type of the original packed matrix
            uintptr_t nCurrPacketIdx = (uintptr_t)oGT.datastart;
            for(size_t s=0; s<vSizes.size(); ++s) {
                const size_t nCurrPacketSize = oGT.elemSize()*vSizes[s].area();
                lvAssert_(nCurrPacketIdx+nCurrPacketSize<uintptr_t(oGT.dataend),"unpack out-of-bounds");
                if(nCurrPacketSize>0)
                    vUnpackedGT[s] = cv::Mat(vSizes[s],oGT.type(),(void*)nCurrPacketIdx);
                else
                    vUnpackedGT[s] = cv::Mat();
                nCurrPacketIdx += nCurrPacketSize;
            }
            lvAssert_(nCurrPacketIdx==uintptr_t(oGT.dataend),"unpack has leftover data");
        }
    }
    else
        lvError("unhandled packet type in unpackGT");
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

const cv::Mat& lv::IDataProducer_<lv::DatasetSource_Video>::getFrameROI() const {
    return m_oInputROI;
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_Video>::getFrameSize() const {
    return m_oInputSize;
}

size_t lv::IDataProducer_<lv::DatasetSource_Video>::getInputCount() const {
    return m_nFrameCount;
}

size_t lv::IDataProducer_<lv::DatasetSource_Video>::getGTCount() const {
    return m_mGTIndexLUT.size();
}

double lv::IDataProducer_<lv::DatasetSource_Video>::getExpectedLoad() const {
    return (double)(m_oInputROI.empty()?m_oInputSize.area():cv::countNonZero(m_oInputROI))*m_nFrameCount*(int(!isGrayscale())+1);
}

void lv::IDataProducer_<lv::DatasetSource_Video>::startPrecaching(bool bPrecacheGT, size_t nSuggestedBufferSize) {
    return IIDataLoader::startPrecaching(bPrecacheGT,(nSuggestedBufferSize==SIZE_MAX)?m_oInputSize.area()*(m_nFrameCount+1)*(isGrayscale()?1:getDatasetInfo()->is4ByteAligned()?4:3):nSuggestedBufferSize);
}

lv::IDataProducer_<lv::DatasetSource_Video>::IDataProducer_(PacketPolicy eGTType, PacketPolicy eOutputType, MappingPolicy eGTMappingType, MappingPolicy eIOMappingType) :
        IDataLoader_<NotArray>(ImagePacket,eGTType,eOutputType,eGTMappingType,eIOMappingType),m_nFrameCount(0),m_nNextExpectedVideoReaderFrameIdx(size_t(-1)) {}

const cv::Mat& lv::IDataProducer_<lv::DatasetSource_Video>::getInputROI(size_t /*nPacketIdx*/) const {
    return m_oInputROI;
}

const cv::Mat& lv::IDataProducer_<lv::DatasetSource_Video>::getGTROI(size_t /*nPacketIdx*/) const {
    return m_oGTROI;
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_Video>::getInputSize(size_t /*nPacketIdx*/) const {
    return m_oInputSize;
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_Video>::getGTSize(size_t /*nPacketIdx*/) const {
    return m_oGTSize;
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_Video>::getInputMaxSize() const {
    return m_oInputSize;
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_Video>::getGTMaxSize() const {
    return m_oGTSize;
}

cv::Mat lv::IDataProducer_<lv::DatasetSource_Video>::getRawInput(size_t nPacketIdx) {
    cv::Mat oFrame;
    if(!m_voVideoReader.isOpened() && nPacketIdx<m_vsInputPaths.size())
        oFrame = cv::imread(m_vsInputPaths[nPacketIdx],isGrayscale()?cv::IMREAD_GRAYSCALE:cv::IMREAD_COLOR);
    else {
        if(m_nNextExpectedVideoReaderFrameIdx!=nPacketIdx) {
            m_voVideoReader.set(cv::CAP_PROP_POS_FRAMES,(double)nPacketIdx);
            m_nNextExpectedVideoReaderFrameIdx = nPacketIdx+1;
        }
        else
            ++m_nNextExpectedVideoReaderFrameIdx;
        m_voVideoReader >> oFrame;
    }
    return oFrame;
}

cv::Mat lv::IDataProducer_<lv::DatasetSource_Video>::getRawGT(size_t nPacketIdx) {
    lvAssert_(getGTPacketType()==ImagePacket,"default impl only works for image gt packets");
    if(m_mGTIndexLUT.count(nPacketIdx)) {
        const size_t nGTIdx = m_mGTIndexLUT[nPacketIdx];
        if(nGTIdx<m_vsGTPaths.size())
            return cv::imread(m_vsGTPaths[nGTIdx],cv::IMREAD_GRAYSCALE); // default = load as grayscale (override if not ok)
    }
    return cv::Mat();
}

void lv::IDataProducer_<lv::DatasetSource_Video>::parseData() {
    cv::Mat oTempImg;
    m_voVideoReader.open(getDataPath());
    if(!m_voVideoReader.isOpened()) {
        lv::GetFilesFromDir(getDataPath(),m_vsInputPaths);
        if(m_vsInputPaths.size()>1) {
            oTempImg = cv::imread(m_vsInputPaths[0]);
            m_nFrameCount = m_vsInputPaths.size();
        }
        else if(m_vsInputPaths.size()==1)
            m_voVideoReader.open(m_vsInputPaths[0]);
    }
    if(m_voVideoReader.isOpened()) {
        m_voVideoReader.set(cv::CAP_PROP_POS_FRAMES,0);
        m_voVideoReader >> oTempImg;
        m_voVideoReader.set(cv::CAP_PROP_POS_FRAMES,0);
        m_nFrameCount = (size_t)m_voVideoReader.get(cv::CAP_PROP_FRAME_COUNT);
    }
    if(oTempImg.empty())
        lvError_("Sequence '%s': video could not be opened via VideoReader or imread (you might need to implement your own DataProducer_ interface)",getName().c_str());
    const double dScale = getDatasetInfo()->getScaleFactor();
    if(dScale!=1.0)
        cv::resize(oTempImg,oTempImg,cv::Size(),dScale,dScale,cv::INTER_NEAREST);
    m_oInputROI = cv::Mat(oTempImg.size(),CV_8UC1,cv::Scalar_<uchar>(255));
    m_oInputSize = oTempImg.size();
    m_nNextExpectedVideoReaderFrameIdx = 0;
    lvAssert_(m_nFrameCount>0,"could not find any input frames");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

const std::vector<cv::Mat>& lv::IDataProducer_<lv::DatasetSource_VideoArray>::getFrameROIArray() const {
    return m_vInputROIs;
}

const std::vector<cv::Size>& lv::IDataProducer_<lv::DatasetSource_VideoArray>::getFrameSizeArray() const {
    return m_vInputSizes;
}

size_t lv::IDataProducer_<lv::DatasetSource_VideoArray>::getInputCount() const {
    return m_vvsInputPaths.size();
}

size_t lv::IDataProducer_<lv::DatasetSource_VideoArray>::getGTCount() const {
    return m_mGTIndexLUT.size();
}

double lv::IDataProducer_<lv::DatasetSource_VideoArray>::getExpectedLoad() const {
    lvAssert_(m_vInputROIs.size()==m_vInputSizes.size(),"internal array sizes mismatch");
    double dLoad = 0.0;
    for(size_t s=0; s<m_vInputSizes.size(); ++s)
        dLoad += (double)(m_vInputROIs[s].empty()?m_vInputSizes[s].area():cv::countNonZero(m_vInputROIs[s]))*m_vvsInputPaths.size()*(int(!isGrayscale())+1);
    return dLoad;
}

void lv::IDataProducer_<lv::DatasetSource_VideoArray>::startPrecaching(bool bPrecacheGT, size_t nSuggestedBufferSize) {
    return IIDataLoader::startPrecaching(bPrecacheGT,(nSuggestedBufferSize==SIZE_MAX)?getInputMaxSize().area()*(m_vvsInputPaths.size()+1)*(isGrayscale()?1:getDatasetInfo()->is4ByteAligned()?4:3):nSuggestedBufferSize);
}

lv::IDataProducer_<lv::DatasetSource_VideoArray>::IDataProducer_(PacketPolicy eGTType, PacketPolicy eOutputType, MappingPolicy eGTMappingType, MappingPolicy eIOMappingType) :
        IDataLoader_<Array>(ImageArrayPacket,eGTType,eOutputType,eGTMappingType,eIOMappingType) {}

const std::vector<cv::Mat>& lv::IDataProducer_<lv::DatasetSource_VideoArray>::getInputROIArray(size_t /*nPacketIdx*/) const {
    return m_vInputROIs;
}

const std::vector<cv::Mat>& lv::IDataProducer_<lv::DatasetSource_VideoArray>::getGTROIArray(size_t /*nPacketIdx*/) const {
    return m_vGTROIs;
}

const std::vector<cv::Size>& lv::IDataProducer_<lv::DatasetSource_VideoArray>::getInputSizeArray(size_t /*nPacketIdx*/) const {
    return m_vInputSizes;
}

const std::vector<cv::Size>& lv::IDataProducer_<lv::DatasetSource_VideoArray>::getGTSizeArray(size_t /*nPacketIdx*/) const {
    return m_vGTSizes;
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_VideoArray>::getInputMaxSize() const {
    return m_oMaxInputSize;
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_VideoArray>::getGTMaxSize() const {
    return m_oMaxGTSize;
}

cv::Mat lv::IDataProducer_<lv::DatasetSource_VideoArray>::getRawInput(size_t nPacketIdx) {
    if(nPacketIdx>=m_vvsInputPaths.size())
        return cv::Mat();
    const std::vector<std::string>& vsInputPaths = m_vvsInputPaths[nPacketIdx];
    if(vsInputPaths.empty())
        return cv::Mat();
    lvAssert_(vsInputPaths.size()==getInputStreamCount(),"input path count did not match stream count");
    const std::vector<cv::Size>& vsInputSizes = getInputSizeArray(nPacketIdx);
    lvAssert_(vsInputPaths.size()==vsInputSizes.size(),"input path count did not match size count");
    cv::Mat oPacket;
    for(size_t nStreamIdx=0; nStreamIdx<vsInputPaths.size(); ++nStreamIdx) {
        cv::Mat oCurrImg = cv::imread(vsInputPaths[nStreamIdx]);
        if(oCurrImg.empty()) // if a single image is missing/cannot load, we skip the entire packet
            return cv::Mat();
        if(getDatasetInfo()->is4ByteAligned() && oCurrImg.channels()==3)
            cv::cvtColor(oCurrImg,oCurrImg,cv::COLOR_BGR2BGRA);
        const cv::Size& oPacketSize = vsInputSizes[nStreamIdx];
        lvAssert_(oPacketSize.area(),"proper per-stream packet size is needed for packing/unpacking");
        if(oCurrImg.size()!=oPacketSize)
            cv::resize(oCurrImg,oCurrImg,oPacketSize,0,0,cv::INTER_NEAREST);
        if(oPacket.empty())
            oPacket = oCurrImg;
        else {
            // default 'packing' strategy for image packets is continuous data concat
            lvAssert(oPacket.isContinuous() && oCurrImg.isContinuous() && oPacket.type()==oCurrImg.type());
            lvDbgAssert(size_t(oPacket.dataend-oPacket.datastart)==oPacket.total()*oPacket.elemSize());
            cv::Mat oNewPacket(1,oPacket.size().area()+oPacketSize.area(),oPacket.type());
            std::copy(oPacket.datastart,oPacket.dataend,oNewPacket.data);
            std::copy(oCurrImg.datastart,oCurrImg.dataend,oNewPacket.data+uintptr_t(oPacket.dataend-oPacket.datastart));
        }
    }
    return oPacket;
}

cv::Mat lv::IDataProducer_<lv::DatasetSource_VideoArray>::getRawGT(size_t nPacketIdx) {
    lvAssert_(getGTPacketType()<=ImageArrayPacket,"default impl only works for image array or image gt packets");
    if(m_mGTIndexLUT.count(nPacketIdx)) {
        const size_t nGTIdx = m_mGTIndexLUT[nPacketIdx];
        if(nGTIdx<m_vvsGTPaths.size()) {
            const std::vector<std::string>& vsGTPaths = m_vvsGTPaths[nGTIdx];
            if(vsGTPaths.empty())
                return cv::Mat();
            lvAssert_(vsGTPaths.size()==getGTStreamCount(),"GT path count did not match stream count");
            if(vsGTPaths.size()==1 && getGTPacketType()==ImagePacket)
                return cv::imread(vsGTPaths[0],cv::IMREAD_GRAYSCALE);
            const std::vector<cv::Size>& vsGTSizes = getGTSizeArray(nGTIdx);
            lvAssert_(vsGTPaths.size()==vsGTSizes.size(),"GT path count did not match size count");
            cv::Mat oPacket;
            for(size_t nStreamIdx=0; nStreamIdx<vsGTPaths.size(); ++nStreamIdx) {
                cv::Mat oCurrImg = cv::imread(vsGTPaths[nStreamIdx],cv::IMREAD_GRAYSCALE); // default = load as grayscale (override if not ok)
                if(oCurrImg.empty()) // if a single image is missing/cannot load, we skip the entire packet
                    return cv::Mat();
                const cv::Size& oPacketSize = vsGTSizes[nStreamIdx];
                lvAssert_(oPacketSize.area(),"proper per-stream packet size is needed for packing/unpacking");
                if(oCurrImg.size()!=oPacketSize)
                    cv::resize(oCurrImg,oCurrImg,oPacketSize,0,0,cv::INTER_NEAREST);
                if(oPacket.empty())
                    oPacket = oCurrImg;
                else {
                    // default 'packing' strategy for image packets is continuous data concat
                    lvAssert(oPacket.isContinuous() && oCurrImg.isContinuous() && oPacket.type()==oCurrImg.type());
                    lvDbgAssert(size_t(oPacket.dataend-oPacket.datastart)==oPacket.total()*oPacket.elemSize());
                    cv::Mat oNewPacket(1,oPacket.size().area()+oPacketSize.area(),oPacket.type());
                    std::copy(oPacket.datastart,oPacket.dataend,oNewPacket.data);
                    std::copy(oCurrImg.datastart,oCurrImg.dataend,oNewPacket.data+uintptr_t(oPacket.dataend-oPacket.datastart));
                }
            }
            return oPacket;
        }
    }
    return cv::Mat();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool lv::IDataProducer_<lv::DatasetSource_Image>::isInputConstantSize() const {
    return m_bIsInputConstantSize;
}

bool lv::IDataProducer_<lv::DatasetSource_Image>::isGTConstantSize() const {
    return m_bIsGTConstantSize;
}

size_t lv::IDataProducer_<lv::DatasetSource_Image>::getInputCount() const {
    return m_vsInputPaths.size();
}

size_t lv::IDataProducer_<lv::DatasetSource_Image>::getGTCount() const {
    return m_mGTIndexLUT.size();
}

double lv::IDataProducer_<lv::DatasetSource_Image>::getExpectedLoad() const {
    return (double)m_oInputMaxSize.area()*m_vsInputPaths.size()*(int(!isGrayscale())+1);
}

void lv::IDataProducer_<lv::DatasetSource_Image>::startPrecaching(bool bPrecacheGT, size_t nSuggestedBufferSize) {
    return IIDataLoader::startPrecaching(bPrecacheGT,(nSuggestedBufferSize==SIZE_MAX)?m_oInputMaxSize.area()*(m_vsInputPaths.size()+1)*(isGrayscale()?1:getDatasetInfo()->is4ByteAligned()?4:3):nSuggestedBufferSize);
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_Image>::getInputSize(size_t nPacketIdx) const {
    if(nPacketIdx>=m_vInputSizes.size())
        return cv::emptySize();
    return m_vInputSizes[nPacketIdx];
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_Image>::getGTSize(size_t nPacketIdx) const {
    if(m_mGTIndexLUT.count(nPacketIdx)) {
        const size_t& nGTIdx = m_mGTIndexLUT.at(nPacketIdx);
        if(nGTIdx<m_vGTSizes.size())
            return m_vGTSizes[nGTIdx];
    }
    return cv::emptySize();
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_Image>::getInputMaxSize() const {
    return m_oInputMaxSize;
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_Image>::getGTMaxSize() const {
    return m_oGTMaxSize;
}

std::string lv::IDataProducer_<lv::DatasetSource_Image>::getInputName(size_t nPacketIdx) const {
    if(nPacketIdx>=m_vsInputPaths.size())
        return IDataHandler::getInputName(nPacketIdx);
    const size_t nLastSlashPos = m_vsInputPaths[nPacketIdx].find_last_of("/\\");
    std::string sFileName = (nLastSlashPos==std::string::npos)?m_vsInputPaths[nPacketIdx]:m_vsInputPaths[nPacketIdx].substr(nLastSlashPos+1);
    return sFileName.substr(0,sFileName.find_last_of("."));
}

lv::IDataProducer_<lv::DatasetSource_Image>::IDataProducer_(PacketPolicy eGTType, PacketPolicy eOutputType, MappingPolicy eGTMappingType, MappingPolicy eIOMappingType) :
        IDataLoader_<NotArray>(ImagePacket,eGTType,eOutputType,eGTMappingType,eIOMappingType) {}

cv::Mat lv::IDataProducer_<lv::DatasetSource_Image>::getRawInput(size_t nPacketIdx) {
    if(nPacketIdx>=m_vsInputPaths.size())
        return cv::Mat();
    return cv::imread(m_vsInputPaths[nPacketIdx],isGrayscale()?cv::IMREAD_GRAYSCALE:cv::IMREAD_COLOR);
}

cv::Mat lv::IDataProducer_<lv::DatasetSource_Image>::getRawGT(size_t nPacketIdx) {
    lvAssert_(getGTPacketType()==ImagePacket,"default impl only works for image gt packets");
    if(m_mGTIndexLUT.count(nPacketIdx)) {
        const size_t nGTIdx = m_mGTIndexLUT[nPacketIdx];
        if(nGTIdx<m_vsGTPaths.size())
            return cv::imread(m_vsGTPaths[nGTIdx],cv::IMREAD_GRAYSCALE); // default = load as grayscale (override if not ok)
    }
    return cv::Mat();
}

void lv::IDataProducer_<lv::DatasetSource_Image>::parseData() {
    lv::GetFilesFromDir(getDataPath(),m_vsInputPaths);
    lv::FilterFilePaths(m_vsInputPaths,{},{".jpg",".png",".bmp"});
    if(m_vsInputPaths.empty())
        lvError_("Set '%s' did not possess any jpg/png/bmp image file",getName().c_str());
    m_bIsInputConstantSize = true;
    m_oInputMaxSize = cv::Size(0,0);
    m_vInputSizes.clear();
    m_vInputSizes.reserve(m_vsInputPaths.size());
    cv::Size oLastSize;
    const double dScale = getDatasetInfo()->getScaleFactor();
    for(size_t n = 0; n<m_vsInputPaths.size(); ++n) {
        cv::Mat oCurrInput = cv::imread(m_vsInputPaths[n],isGrayscale()?cv::IMREAD_GRAYSCALE:cv::IMREAD_COLOR);
        while(oCurrInput.empty()) {
            m_vsInputPaths.erase(m_vsInputPaths.begin()+n);
            if(n>=m_vsInputPaths.size())
                break;
            oCurrInput = cv::imread(m_vsInputPaths[n],isGrayscale()?cv::IMREAD_GRAYSCALE:cv::IMREAD_COLOR);
        }
        if(oCurrInput.empty())
            break;
        if(dScale!=1.0)
            cv::resize(oCurrInput,oCurrInput,cv::Size(),dScale,dScale,cv::INTER_NEAREST);
        m_vInputSizes.push_back(oCurrInput.size());
        m_oInputMaxSize.width = std::max(oCurrInput.cols,m_oInputMaxSize.width);
        m_oInputMaxSize.height = std::max(oCurrInput.rows,m_oInputMaxSize.height);
        if(oLastSize.area() && oCurrInput.size()!=oLastSize)
            m_bIsInputConstantSize = false;
        oLastSize = oCurrInput.size();
    }
    lvAssert_(!m_vInputSizes.empty(),"could not find any input images");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool lv::IDataProducer_<lv::DatasetSource_ImageArray>::isInputConstantSize() const {
    return m_bIsInputConstantSize;
}

bool lv::IDataProducer_<lv::DatasetSource_ImageArray>::isGTConstantSize() const {
    return m_bIsGTConstantSize;
}

size_t lv::IDataProducer_<lv::DatasetSource_ImageArray>::getInputCount() const {
    return m_vvsInputPaths.size();
}

size_t lv::IDataProducer_<lv::DatasetSource_ImageArray>::getGTCount() const {
    return m_mGTIndexLUT.size();
}

double lv::IDataProducer_<lv::DatasetSource_ImageArray>::getExpectedLoad() const {
    return (double)m_oInputMaxSize.area()*m_vvsInputPaths.size()*(int(!isGrayscale())+1);
}

void lv::IDataProducer_<lv::DatasetSource_ImageArray>::startPrecaching(bool bPrecacheGT, size_t nSuggestedBufferSize) {
    return IIDataLoader::startPrecaching(bPrecacheGT,(nSuggestedBufferSize==SIZE_MAX)?m_oInputMaxSize.area()*(m_vvsInputPaths.size()+1)*(isGrayscale()?1:getDatasetInfo()->is4ByteAligned()?4:3):nSuggestedBufferSize);
}

const std::vector<cv::Size>& lv::IDataProducer_<lv::DatasetSource_ImageArray>::getInputSizeArray(size_t nPacketIdx) const {
    if(nPacketIdx>=m_vvInputSizes.size())
        return cv::emptySizeArray();
    return m_vvInputSizes[nPacketIdx];
}

const std::vector<cv::Size>& lv::IDataProducer_<lv::DatasetSource_ImageArray>::getGTSizeArray(size_t nPacketIdx) const {
    if(m_mGTIndexLUT.count(nPacketIdx)) {
        const size_t& nGTIdx = m_mGTIndexLUT.at(nPacketIdx);
        if(nGTIdx<m_vvGTSizes.size())
            return m_vvGTSizes[nGTIdx];
    }
    return cv::emptySizeArray();
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_ImageArray>::getInputMaxSize() const {
    return m_oInputMaxSize;
}

const cv::Size& lv::IDataProducer_<lv::DatasetSource_ImageArray>::getGTMaxSize() const {
    return m_oGTMaxSize;
}

lv::IDataProducer_<lv::DatasetSource_ImageArray>::IDataProducer_(PacketPolicy eGTType, PacketPolicy eOutputType, MappingPolicy eGTMappingType, MappingPolicy eIOMappingType) :
        IDataLoader_<Array>(ImagePacket,eGTType,eOutputType,eGTMappingType,eIOMappingType) {}

cv::Mat lv::IDataProducer_<lv::DatasetSource_ImageArray>::getRawInput(size_t nPacketIdx) {
    if(nPacketIdx>=m_vvsInputPaths.size())
        return cv::Mat();
    const std::vector<std::string>& vsInputPaths = m_vvsInputPaths[nPacketIdx];
    if(vsInputPaths.empty())
        return cv::Mat();
    lvAssert_(vsInputPaths.size()==getInputStreamCount(),"input path count did not match stream count");
    const std::vector<cv::Size>& vsInputSizes = getInputSizeArray(nPacketIdx);
    lvAssert_(vsInputPaths.size()==vsInputSizes.size(),"input path count did not match size count");
    cv::Mat oPacket;
    for(size_t nStreamIdx=0; nStreamIdx<vsInputPaths.size(); ++nStreamIdx) {
        cv::Mat oCurrImg = cv::imread(vsInputPaths[nStreamIdx]);
        if(oCurrImg.empty()) // if a single image is missing/cannot load, we skip the entire packet
            return cv::Mat();
        if(getDatasetInfo()->is4ByteAligned() && oCurrImg.channels()==3)
            cv::cvtColor(oCurrImg,oCurrImg,cv::COLOR_BGR2BGRA);
        const cv::Size& oPacketSize = vsInputSizes[nStreamIdx];
        lvAssert_(oPacketSize.area(),"proper per-stream packet size is needed for packing/unpacking");
        if(oCurrImg.size()!=oPacketSize)
            cv::resize(oCurrImg,oCurrImg,oPacketSize,0,0,cv::INTER_NEAREST);
        if(oPacket.empty())
            oPacket = oCurrImg;
        else {
            // default 'packing' strategy for image packets is continuous data concat
            lvAssert(oPacket.isContinuous() && oCurrImg.isContinuous() && oPacket.type()==oCurrImg.type());
            lvDbgAssert(size_t(oPacket.dataend-oPacket.datastart)==oPacket.total()*oPacket.elemSize());
            cv::Mat oNewPacket(1,oPacket.size().area()+oPacketSize.area(),oPacket.type());
            std::copy(oPacket.datastart,oPacket.dataend,oNewPacket.data);
            std::copy(oCurrImg.datastart,oCurrImg.dataend,oNewPacket.data+uintptr_t(oPacket.dataend-oPacket.datastart));
        }
    }
    return oPacket;
}

cv::Mat lv::IDataProducer_<lv::DatasetSource_ImageArray>::getRawGT(size_t nPacketIdx) {
    lvAssert_(getGTPacketType()<=ImageArrayPacket,"default impl only works for image array or image gt packets");
    if(m_mGTIndexLUT.count(nPacketIdx)) {
        const size_t nGTIdx = m_mGTIndexLUT[nPacketIdx];
        if(nGTIdx<m_vvsGTPaths.size()) {
            const std::vector<std::string>& vsGTPaths = m_vvsGTPaths[nGTIdx];
            if(vsGTPaths.empty())
                return cv::Mat();
            lvAssert_(vsGTPaths.size()==getGTStreamCount(),"GT path count did not match stream count");
            if(vsGTPaths.size()==1 && getGTPacketType()==ImagePacket)
                return cv::imread(vsGTPaths[0],cv::IMREAD_GRAYSCALE);
            const std::vector<cv::Size>& vsGTSizes = getGTSizeArray(nGTIdx);
            lvAssert_(vsGTPaths.size()==vsGTSizes.size(),"GT path count did not match size count");
            cv::Mat oPacket;
            for(size_t nStreamIdx=0; nStreamIdx<vsGTPaths.size(); ++nStreamIdx) {
                cv::Mat oCurrImg = cv::imread(vsGTPaths[nStreamIdx],cv::IMREAD_GRAYSCALE); // default = load as grayscale (override if not ok)
                if(oCurrImg.empty()) // if a single image is missing/cannot load, we skip the entire packet
                    return cv::Mat();
                const cv::Size& oPacketSize = vsGTSizes[nStreamIdx];
                lvAssert_(oPacketSize.area(),"proper per-stream packet size is needed for packing/unpacking");
                if(oCurrImg.size()!=oPacketSize)
                    cv::resize(oCurrImg,oCurrImg,oPacketSize,0,0,cv::INTER_NEAREST);
                if(oPacket.empty())
                    oPacket = oCurrImg;
                else {
                    // default 'packing' strategy for image packets is continuous data concat
                    lvAssert(oPacket.isContinuous() && oCurrImg.isContinuous() && oPacket.type()==oCurrImg.type());
                    lvDbgAssert(size_t(oPacket.dataend-oPacket.datastart)==oPacket.total()*oPacket.elemSize());
                    cv::Mat oNewPacket(1,oPacket.size().area()+oPacketSize.area(),oPacket.type());
                    std::copy(oPacket.datastart,oPacket.dataend,oNewPacket.data);
                    std::copy(oCurrImg.datastart,oCurrImg.dataend,oNewPacket.data+uintptr_t(oPacket.dataend-oPacket.datastart));
                }
            }
            return oPacket;
        }
    }
    return cv::Mat();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

size_t lv::IDataCounter_<lv::NotGroup>::getProcessedOutputCount() const {
    return m_mProcessedPackets.size();
}

size_t lv::IDataCounter_<lv::NotGroup>::getProcessedOutputCountPromise() {
    return m_nProcessedPacketsPromise.get_future().get();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

size_t lv::IDataCounter_<lv::Group>::getExpectedOutputCount() const {
    return lv::accumulateMembers<size_t,IDataHandlerPtr>(getBatches(true),[](const IDataHandlerPtr& p){return p->getExpectedOutputCount();});
}

size_t lv::IDataCounter_<lv::Group>::getProcessedOutputCount() const {
    return lv::accumulateMembers<size_t,IDataHandlerPtr>(getBatches(true),[](const IDataHandlerPtr& p){return p->getProcessedOutputCount();});
}

size_t lv::IDataCounter_<lv::Group>::getProcessedOutputCountPromise() {
    return lv::accumulateMembers<size_t,IDataHandlerPtr>(getBatches(true),[](const IDataHandlerPtr& p){return p->getProcessedOutputCountPromise();});
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

lv::DataWriter::DataWriter(std::function<size_t(const cv::Mat&,size_t)> lDataArchiverCallback) :
        m_lCallback(lDataArchiverCallback) {
    lvAssert_(m_lCallback,"invalid data writer callback");
    m_bIsActive = false;
    m_bAllowPacketDrop = false;
    m_nQueueSize = 0;
    m_nQueueCount = 0;
}

lv::DataWriter::~DataWriter() {
    stopAsyncWriting();
}

size_t lv::DataWriter::queue(const cv::Mat& oPacket, size_t nIdx) {
    if(!m_bIsActive)
        return m_lCallback(oPacket,nIdx);
    cv::Mat oPacketCopy = oPacket.clone();
    const size_t nPacketSize = oPacket.total()*oPacket.elemSize();
    size_t nPacketPosition;
    {
        std::mutex_unique_lock sync_lock(m_oSyncMutex);
        if(!m_bAllowPacketDrop && m_nQueueSize+nPacketSize>m_nQueueMaxSize)
            m_oClearCondVar.wait(sync_lock,[&]{return m_nQueueSize+nPacketSize<=m_nQueueMaxSize;});
        if(m_nQueueSize+nPacketSize<=m_nQueueMaxSize) {
            m_mQueue[nIdx] = std::move(oPacketCopy);
            m_nQueueSize += nPacketSize;
            // @@@ could cut a find operation here using C++17's map::insert_or_assign above
            nPacketPosition = std::distance(m_mQueue.begin(),m_mQueue.find(nIdx));
            ++m_nQueueCount;
        }
        else {
#if CONSOLE_DEBUG
            std::cout << "data writer [" << uintptr_t(this) << "] dropping packet #" << nIdx << std::endl;
#endif //CONSOLE_DEBUG
            nPacketPosition = SIZE_MAX; // packet dropped
        }
    }
    m_oQueueCondVar.notify_one();
#if CONSOLE_DEBUG
    if((nIdx%50)==0)
        std::cout << "data writer [" << uintptr_t(this) << "] queue @ " << (int)(((float)m_nQueueSize*100)/m_nQueueMaxSize) << "% capacity" << std::endl;
#endif //CONSOLE_DEBUG
    return nPacketPosition;
}

bool lv::DataWriter::startAsyncWriting(size_t nSuggestedQueueSize, bool bDropPacketsIfFull, size_t nWorkers) {
    if(m_bIsActive)
        stopAsyncWriting();
    if(nSuggestedQueueSize>0) {
        m_bIsActive = true;
        m_bAllowPacketDrop = bDropPacketsIfFull;
        m_nQueueMaxSize = (nSuggestedQueueSize>CACHE_MAX_SIZE)?(CACHE_MAX_SIZE):nSuggestedQueueSize;
        m_nQueueSize = 0;
        m_nQueueCount = 0;
        m_mQueue.clear();
        for(size_t n=0; n<nWorkers; ++n)
            m_vhWorkers.emplace_back(std::bind(&DataWriter::entry,this));
    }
    return m_bIsActive;
}

void lv::DataWriter::stopAsyncWriting() {
    if(m_bIsActive) {
        m_bIsActive = false;
        m_oQueueCondVar.notify_all();
        for(std::thread& oWorker : m_vhWorkers)
            oWorker.join();
    }
}

void lv::DataWriter::entry() {
    std::mutex_unique_lock sync_lock(m_oSyncMutex);
#if CONSOLE_DEBUG
    std::cout << "data writer [" << uintptr_t(this) << "] init w/ max buffer size = " << (m_nQueueMaxSize/1024)/1024 << " mb" << std::endl;
#endif //CONSOLE_DEBUG
    while(m_bIsActive || m_nQueueCount>0) {
        if(m_nQueueCount==0)
            m_oQueueCondVar.wait(sync_lock);
        if(m_nQueueCount>0) {
            auto pCurrPacket = m_mQueue.begin();
            lvAssert_(pCurrPacket!=m_mQueue.end(),"data writer notified for missing packet");
            const cv::Mat oPacketData(std::move(pCurrPacket->second));
            const size_t nPacketIdx = pCurrPacket->first;
            const size_t nPacketSize = oPacketData.total()*oPacketData.elemSize();
            lvAssert_(nPacketSize<=m_nQueueSize,"data writer packet size exceeds queue size");
            m_nQueueSize -= nPacketSize;
            m_mQueue.erase(pCurrPacket);
            --m_nQueueCount;
            std::unlock_guard<std::mutex_unique_lock> oUnlock(sync_lock);
            m_lCallback(oPacketData,nPacketIdx);
            m_oClearCondVar.notify_all();
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

void lv::IDataArchiver_<lv::NotArray>::save(const cv::Mat& oOutput, size_t nIdx, int /*nFlags*/) {
    const auto pLoader = shared_from_this_cast<const IIDataLoader>(true);
    if(pLoader->getOutputPacketType()==ImagePacket) {
        lvAssert_(!getDatasetInfo()->getOutputNameSuffix().empty(),"data archiver requires image packet output name suffix (i.e. file extension)");
        std::stringstream sOutputFilePath;
        sOutputFilePath << getOutputPath() << getDatasetInfo()->getOutputNamePrefix() << getOutputName(nIdx) << getDatasetInfo()->getOutputNameSuffix();
        cv::Mat oOutputClone = oOutput.clone();
        // automatically gray-out zones outside ROI if output is binary image mask with 1:1 mapping (e.g. segmentation)
        if(pLoader->getGTPacketType()==ImagePacket && pLoader->getGTMappingType()==PixelMapping && oOutput.type()==CV_8UC1 && (cv::countNonZero(oOutput==UCHAR_MAX)+cv::countNonZero(oOutput==0))==oOutput.size().area()) {
            const cv::Mat& oROI = pLoader->getGTROI(nIdx);
            if(!oROI.empty() && oROI.size()==oOutputClone.size()) {
                cv::bitwise_or(oOutputClone,UCHAR_MAX/2,oOutputClone,oROI==0);
                cv::bitwise_and(oOutputClone,UCHAR_MAX/2,oOutputClone,oROI==0);
            }
        }
        const std::vector<int> vnComprParams = {cv::IMWRITE_PNG_COMPRESSION,9};
        cv::imwrite(sOutputFilePath.str(),oOutputClone,vnComprParams);
    }
    else {
        // @@@@ save to YML/bin file?
        lvError("Missing lv::IDataArchiver::save override impl");
    }
}

cv::Mat lv::IDataArchiver_<lv::NotArray>::load(size_t nIdx, int nFlags) {
    const auto pLoader = shared_from_this_cast<const IIDataLoader>(true);
    if(pLoader->getOutputPacketType()==ImagePacket) {
        lvAssert_(!getDatasetInfo()->getOutputNameSuffix().empty(),"data archiver requires packet output name suffix (i.e. file extension)");
        std::stringstream sOutputFilePath;
        sOutputFilePath << getOutputPath() << getDatasetInfo()->getOutputNamePrefix() << getOutputName(nIdx) << getDatasetInfo()->getOutputNameSuffix();
        return cv::imread(sOutputFilePath.str(),(nFlags==-1)?cv::IMREAD_GRAYSCALE:cv::IMREAD_COLOR);
    }
    else {
        // @@@@ read from YML/bin file?
        lvError("Missing lv::IDataArchiver::load override impl");
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void lv::IDataArchiver_<lv::Array>::saveArray(const std::vector<cv::Mat>& /*vOutput*/, size_t /*nIdx*/, int /*nFlags*/) {
    // @@@@ save to YML/bin file?
    lvError("Missing lv::IDataArchiver::saveArray override impl");

}

std::vector<cv::Mat> lv::IDataArchiver_<lv::Array>::loadArray(size_t /*nIdx*/, int /*nFlags*/) {
    // @@@@ read from YML/bin file?
    lvError("Missing lv::IDataArchiver::loadArray override impl");
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

#if HAVE_GLSL

cv::Size lv::IAsyncDataConsumer_<lv::DatasetEval_BinaryClassifier,lv::GLSL>::getIdealGLWindowSize() const {
    lvAssert_(getExpectedOutputCount()>1,"async data consumer requires work batch to expect more than one output packet");
    cv::Size oWindowSize = shared_from_this_cast<const IDataLoader_<NotArray>>(true)->getInputMaxSize();
    lvAssert_(oWindowSize.area(),"max input size must be non-null");
    if(m_pEvalAlgo) {
        lvAssert_(m_pEvalAlgo->getIsGLInitialized(),"evaluator algo must be initialized first");
        oWindowSize.width *= int(m_pEvalAlgo->m_nSxSDisplayCount);
    }
    else if(m_pAlgo) {
        lvAssert_(m_pAlgo->getIsGLInitialized(),"algo must be initialized first");
        oWindowSize.width *= int(m_pAlgo->m_nSxSDisplayCount);
    }
    return oWindowSize;
}

lv::IAsyncDataConsumer_<lv::DatasetEval_BinaryClassifier,lv::GLSL>::IAsyncDataConsumer_() :
        m_nLastIdx(0),
        m_nCurrIdx(0),
        m_nNextIdx(1) {}

void lv::IAsyncDataConsumer_<lv::DatasetEval_BinaryClassifier,lv::GLSL>::pre_initialize_gl() {
    lvAssert_(getExpectedOutputCount()>1,"async data consumer requires work batch to expect more than one output packet");
    m_pLoader = shared_from_this_cast<IIDataLoader>(true);
    lvAssert_(m_pLoader->getInputPacketType()==ImagePacket && m_pLoader->getOutputPacketType()==ImagePacket && m_pLoader->getIOMappingType()==PixelMapping,"async data consumer only defined to work with image packets under 1:1 mapping");
    lvAssert_(m_pAlgo,"invalid algo given to async data consumer");
    m_oCurrInput = m_pLoader->getInput(m_nCurrIdx).clone();
    m_oNextInput = m_pLoader->getInput(m_nNextIdx).clone();
    m_oLastInput = m_oCurrInput.clone();
    lvAssert_(!m_oCurrInput.empty() && m_oCurrInput.isContinuous(),"invalid input fetched from loader");
    lvAssert_(m_oCurrInput.channels()==1 || m_oCurrInput.channels()==4,"loaded data must be 1ch or 4ch to avoid alignment problems");
    if(getDatasetInfo()->isSavingOutput() || m_pAlgo->m_pDisplayHelper)
        m_pAlgo->setOutputFetching(true);
    if(m_pAlgo->m_pDisplayHelper && m_pAlgo->m_bUsingDebug)
        m_pAlgo->setDebugFetching(true);
    if(getDatasetInfo()->isUsingEvaluator()) {
        lvAssert_(m_pLoader->getGTPacketType()==ImagePacket && m_pLoader->getGTMappingType()==PixelMapping,"async data consumer only defined to work with gt image packets under 1:1 mapping");
        m_oCurrGT = m_pLoader->getGT(m_nCurrIdx).clone();
        m_oNextGT = m_pLoader->getGT(m_nNextIdx).clone();
        m_oLastGT = m_oCurrGT.clone();
        lvAssert_(!m_oCurrGT.empty() && m_oCurrGT.isContinuous(),"invalid gt fetched from loader");
        lvAssert_(m_oCurrGT.channels()==1 || m_oCurrGT.channels()==4,"gt data must be 1ch or 4ch to avoid alignment problems");
    }
}

void lv::IAsyncDataConsumer_<lv::DatasetEval_BinaryClassifier,lv::GLSL>::post_initialize_gl() {
    lvDbgAssert(m_pAlgo);
}

void lv::IAsyncDataConsumer_<lv::DatasetEval_BinaryClassifier,lv::GLSL>::pre_apply_gl(size_t nNextIdx, bool bRebindAll) {
    UNUSED(bRebindAll);
    lvDbgAssert_(m_pLoader,"invalid data loader given to async data consumer");
    lvDbgAssert_(m_pAlgo,"invalid algo given to async data consumer");
    if(nNextIdx!=m_nNextIdx)
        m_oNextInput = m_pLoader->getInput(nNextIdx);
    if(getDatasetInfo()->isUsingEvaluator() && nNextIdx!=m_nNextIdx)
        m_oNextGT = m_pLoader->getGT(nNextIdx);
}

void lv::IAsyncDataConsumer_<lv::DatasetEval_BinaryClassifier,lv::GLSL>::post_apply_gl(size_t nNextIdx, bool bRebindAll) {
    lvDbgAssert(m_pLoader && m_pAlgo);
    if(m_pEvalAlgo && getDatasetInfo()->isUsingEvaluator())
        m_pEvalAlgo->apply_gl(m_oNextGT,bRebindAll);
    m_nLastIdx = m_nCurrIdx;
    m_nCurrIdx = nNextIdx;
    m_nNextIdx = nNextIdx+1;
    if(m_pAlgo->m_pDisplayHelper || m_lDataCallback) {
        m_oCurrInput.copyTo(m_oLastInput);
        m_oNextInput.copyTo(m_oCurrInput);
        if(getDatasetInfo()->isUsingEvaluator()) {
            m_oCurrGT.copyTo(m_oLastGT);
            m_oNextGT.copyTo(m_oCurrGT);
        }
    }
    if(m_nNextIdx<getInputCount()) {
        m_oNextInput = m_pLoader->getInput(m_nNextIdx);
        if(getDatasetInfo()->isUsingEvaluator())
            m_oNextGT = m_pLoader->getGT(m_nNextIdx);
    }
    if(getDatasetInfo()->isSavingOutput() || m_pAlgo->m_pDisplayHelper || m_lDataCallback) {
        cv::Mat oLastOutput,oLastDebug;
        m_pAlgo->fetchLastOutput(oLastOutput);
        if(m_pAlgo->m_pDisplayHelper && m_pEvalAlgo && m_pEvalAlgo->m_bUsingDebug)
            m_pEvalAlgo->fetchLastDebug(oLastDebug);
        else if(m_pAlgo->m_pDisplayHelper && m_pAlgo->m_bUsingDebug)
            m_pAlgo->fetchLastDebug(oLastDebug);
        else
            oLastDebug = oLastOutput.clone();
        countOutput(m_nLastIdx);
        if(m_lDataCallback)
            m_lDataCallback(m_oLastInput,oLastDebug,oLastOutput,m_oLastGT,m_pLoader->getGTROI(m_nLastIdx),m_nLastIdx);
        if(getDatasetInfo()->isSavingOutput() && !oLastOutput.empty())
            save(oLastOutput,m_nLastIdx);
        if(m_pAlgo->m_pDisplayHelper && m_pLoader->getGTPacketType()==ImagePacket && m_pLoader->getGTMappingType()==PixelMapping) {
            getColoredMasks(oLastOutput,oLastDebug,m_oLastGT,m_pLoader->getGTROI(m_nLastIdx));
            m_pAlgo->m_pDisplayHelper->display(m_oLastInput,oLastDebug,oLastOutput,m_nLastIdx);
        }
    }
}

void lv::IAsyncDataConsumer_<lv::DatasetEval_BinaryClassifier,lv::GLSL>::getColoredMasks(cv::Mat& oOutput, cv::Mat& oDebug, const cv::Mat& /*oGT*/, const cv::Mat& oGTROI) {
    if(!oGTROI.empty()) {
        lvAssert_(oOutput.size()==oGTROI.size(),"output mat size must match gt ROI size");
        cv::bitwise_or(oOutput,UCHAR_MAX/2,oOutput,oGTROI==0);
        if(!oDebug.empty()) {
            lvAssert_(oDebug.size()==oGTROI.size(),"debug mat size must match gt ROI size");
            cv::bitwise_or(oDebug,UCHAR_MAX/2,oDebug,oGTROI==0);
        }
    }
}

#endif //HAVE_GLSL
