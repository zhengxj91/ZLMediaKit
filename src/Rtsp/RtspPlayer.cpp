﻿/*
 * MIT License
 *
 * Copyright (c) 2016 xiongziliang <771730766@qq.com>
 * Copyright (c) 2018 huohuo <913481084@qq.com>
 *
 * This file is part of ZLMediaKit(https://github.com/xiongziliang/ZLMediaKit).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <set>
#include <cmath>
#include <stdarg.h>
#include <algorithm>

#include "Common/config.h"
#include "RtspPlayer.h"
#include "H264/SPSParser.h"
#include "Util/MD5.h"
#include "Util/mini.h"
#include "Util/util.h"
#include "Util/base64.h"
#include "Network/sockutil.h"
using namespace toolkit;

namespace mediakit {

#define POP_HEAD(trackidx) \
		auto it = _amapRtpSort[trackidx].begin(); \
		onRecvRTP_l(it->second, trackidx); \
		_amapRtpSort[trackidx].erase(it);

#define RTP_BUF_SIZE (4 * 1024)

const char kRtspMd5Nonce[] = "rtsp_md5_nonce";
const char kRtspRealm[] = "rtsp_realm";

RtspPlayer::RtspPlayer(void){
	_pktPool.setSize(64);
}
RtspPlayer::~RtspPlayer(void) {
    DebugL<<endl;
}
void RtspPlayer::teardown(){
	if (alive()) {
		sendRtspRequest("TEARDOWN" ,_strContentBase);
		shutdown();
	}

    erase(kRtspMd5Nonce);
    erase(kRtspRealm);
	_aTrackInfo.clear();
    _onHandshake = nullptr;
    _strSession.clear();
    _uiCseq = 1;
    _strContentBase.clear();
    CLEAR_ARR(_apUdpSock);
    CLEAR_ARR(_aui16LastSeq)
    CLEAR_ARR(_aui16FirstSeq)
    CLEAR_ARR(_aui32SsrcErrorCnt)
    CLEAR_ARR(_aui64RtpRecv)
    CLEAR_ARR(_aui64SeqOkCnt)
    CLEAR_ARR(_abSortStarted)
    CLEAR_ARR(_aui64RtpRecv)
    CLEAR_ARR(_aui16NowSeq)
    _amapRtpSort[0].clear();
    _amapRtpSort[1].clear();

    _pBeatTimer.reset();
    _pPlayTimer.reset();
    _pRtpTimer.reset();
    _iSeekTo = 0;
    CLEAR_ARR(_aiFistStamp);
    CLEAR_ARR(_aiNowStamp);
}

void RtspPlayer::play(const char* strUrl){
	auto userAndPwd = FindField(strUrl,"://","@");
	eRtpType eType = (eRtpType)(int)(*this)[kRtpType];
	if(userAndPwd.empty()){
		play(strUrl,nullptr,nullptr,eType);
		return;
	}
	auto suffix = FindField(strUrl,"@",nullptr);
	auto url = StrPrinter << "rtsp://" << suffix << endl;
	if(userAndPwd.find(":") == string::npos){
		play(url.data(),userAndPwd.data(),nullptr,eType);
		return;
	}
	auto user = FindField(userAndPwd.data(),nullptr,":");
	auto pwd = FindField(userAndPwd.data(),":",nullptr);
	play(url.data(),user.data(),pwd.data(),eType);
}
//播放，指定是否走rtp over tcp
void RtspPlayer::play(const char* strUrl, const char *strUser, const char *strPwd,  eRtpType eType ) {
	DebugL   << strUrl << " "
			<< (strUser ? strUser : "null") << " "
			<< (strPwd ? strPwd:"null") << " "
			<< eType;
	teardown();

	if(strUser){
        (*this)[kRtspUser] = strUser;
    }
    if(strPwd){
        (*this)[kRtspPwd] = strPwd;
		(*this)[kRtspPwdIsMD5] = false;
    }

	_eType = eType;

	auto ip = FindField(strUrl, "://", "/");
	if (!ip.size()) {
		ip = FindField(strUrl, "://", NULL);
	}
	auto port = atoi(FindField(ip.c_str(), ":", NULL).c_str());
	if (port <= 0) {
		//rtsp 默认端口554
		port = 554;
	} else {
		//服务器域名
		ip = FindField(ip.c_str(), NULL, ":");
	}

	_strUrl = strUrl;
	if(!(*this)[PlayerBase::kNetAdapter].empty()){
		setNetAdapter((*this)[PlayerBase::kNetAdapter]);
	}
	startConnect(ip.data(), port);
}
void RtspPlayer::onConnect(const SockException &err){
	if(err.getErrCode()!=Err_success) {
		onPlayResult_l(err);
		teardown();
		return;
	}

	sendDescribe();

	weak_ptr<RtspPlayer> weakSelf = dynamic_pointer_cast<RtspPlayer>(shared_from_this());
	_pPlayTimer.reset( new Timer(10,  [weakSelf]() {
		auto strongSelf=weakSelf.lock();
		if(!strongSelf) {
			return false;
		}
		strongSelf->onPlayResult_l(SockException(Err_timeout,"play rtsp timeout"));
		strongSelf->teardown();
		return false;
	},getExecutor()));
}

void RtspPlayer::onRecv(const Buffer::Ptr& pBuf) {
    input(pBuf->data(),pBuf->size());
}
void RtspPlayer::onErr(const SockException &ex) {
	onShutdown_l (ex);
}
// from live555
bool RtspPlayer::handleAuthenticationFailure(const string &paramsStr) {
    if(!(*this)[kRtspRealm].empty()){
        //已经认证过了
        return false;
    }

    char *realm = new char[paramsStr.size()];
    char *nonce = new char[paramsStr.size()];
    char *stale = new char[paramsStr.size()];
    onceToken token(nullptr,[&](){
        delete[] realm;
        delete[] nonce;
        delete[] stale;
    });

    if (sscanf(paramsStr.data(), "Digest realm=\"%[^\"]\", nonce=\"%[^\"]\", stale=%[a-zA-Z]", realm, nonce, stale) == 3) {
        (*this)[kRtspRealm] = (const char *)realm;
        (*this)[kRtspMd5Nonce] = (const char *)nonce;
        return true;
    }
    if (sscanf(paramsStr.data(), "Digest realm=\"%[^\"]\", nonce=\"%[^\"]\"", realm, nonce) == 2) {
        (*this)[kRtspRealm] = (const char *)realm;
        (*this)[kRtspMd5Nonce] = (const char *)nonce;
        return true;
    }
    if (sscanf(paramsStr.data(), "Basic realm=\"%[^\"]\"", realm) == 1) {
        (*this)[kRtspRealm] = (const char *)realm;
        return true;
    }
    return false;
}
void RtspPlayer::handleResDESCRIBE(const Parser& parser) {
	string authInfo = parser["WWW-Authenticate"];
	//发送DESCRIBE命令后的回复
	if ((parser.Url() == "401") && handleAuthenticationFailure(authInfo)) {
		sendDescribe();
		return;
	}
	if (parser.Url() != "200") {
		throw std::runtime_error(
		StrPrinter << "DESCRIBE:" << parser.Url() << " " << parser.Tail() << endl);
	}
	_strContentBase = parser["Content-Base"];

    if(_strContentBase.empty()){
        _strContentBase = _strUrl;
    }
    if (_strContentBase.back() == '/') {
        _strContentBase.pop_back();
    }

	//解析sdp
	_sdpAttr.load(parser.Content());
	_aTrackInfo = _sdpAttr.getAvailableTrack();

	if (_aTrackInfo.empty()) {
		throw std::runtime_error("无有效的Sdp Track");
	}
	if (!onCheckSDP(parser.Content(), _sdpAttr)) {
		throw std::runtime_error("onCheckSDP faied");
	}

	CLEAR_ARR(_aui32SsrcErrorCnt)
	sendSetup(0);
}
//发送SETUP命令
bool RtspPlayer::sendSetup(unsigned int trackIndex) {
    _onHandshake = std::bind(&RtspPlayer::handleResSETUP,this, placeholders::_1,trackIndex);

    auto &track = _aTrackInfo[trackIndex];
	auto baseUrl = _strContentBase + "/" + track->_control_surffix;
	switch (_eType) {
		case RTP_TCP: {
			StrCaseMap header;
			header["Transport"] = StrPrinter << "RTP/AVP/TCP;unicast;interleaved=" << track->_type * 2 << "-" << track->_type * 2 + 1;
			return sendRtspRequest("SETUP",baseUrl,header);
		}
			break;
		case RTP_MULTICAST: {
			StrCaseMap header;
			header["Transport"] = "Transport: RTP/AVP;multicast";
			return sendRtspRequest("SETUP",baseUrl,header);
		}
			break;
		case RTP_UDP: {
			_apUdpSock[trackIndex].reset(new Socket());
			if (!_apUdpSock[trackIndex]->bindUdpSock(0, get_local_ip().data())) {
				_apUdpSock[trackIndex].reset();
				throw std::runtime_error("open udp sock err");
			}
			int port = _apUdpSock[trackIndex]->get_local_port();
			StrCaseMap header;
			header["Transport"] = StrPrinter << "RTP/AVP;unicast;client_port=" << port << "-" << port + 1;
			return sendRtspRequest("SETUP",baseUrl,header);
		}
			break;
		default:
			return false;
			break;
	}
}

void RtspPlayer::handleResSETUP(const Parser &parser, unsigned int uiTrackIndex) {
	if (parser.Url() != "200") {
		throw std::runtime_error(
		StrPrinter << "SETUP:" << parser.Url() << " " << parser.Tail() << endl);
	}
	if (uiTrackIndex == 0) {
		_strSession = parser["Session"];
        _strSession.append(";");
        _strSession = FindField(_strSession.data(), nullptr, ";");
	}

	auto strTransport = parser["Transport"];
	if(strTransport.find("TCP") != string::npos){
		_eType = RTP_TCP;
	}else if(strTransport.find("multicast") != string::npos){
		_eType = RTP_MULTICAST;
	}else{
		_eType = RTP_UDP;
	}

	RtspSplitter::enableRecvRtp(_eType == RTP_TCP);

	if(_eType == RTP_TCP)  {
		string interleaved = FindField( FindField((strTransport + ";").c_str(), "interleaved=", ";").c_str(), NULL, "-");
		_aTrackInfo[uiTrackIndex]->_interleaved = atoi(interleaved.c_str());
	}else{
		const char *strPos = (_eType == RTP_MULTICAST ? "port=" : "server_port=") ;
		auto port_str = FindField((strTransport + ";").c_str(), strPos, ";");
		uint16_t port = atoi(FindField(port_str.c_str(), NULL, "-").c_str());
        auto &pUdpSockRef = _apUdpSock[uiTrackIndex];
        if(!pUdpSockRef){
            pUdpSockRef.reset(new Socket());
        }
        
		if (_eType == RTP_MULTICAST) {
			auto multiAddr = FindField((strTransport + ";").c_str(), "destination=", ";");
			if (!pUdpSockRef->bindUdpSock(port, "0.0.0.0")) {
				pUdpSockRef.reset();
				throw std::runtime_error("open udp sock err");
			}
			auto fd = pUdpSockRef->rawFD();
			if (-1 == SockUtil::joinMultiAddrFilter(fd, multiAddr.data(), get_peer_ip().data(),get_local_ip().data())) {
				SockUtil::joinMultiAddr(fd, multiAddr.data(),get_local_ip().data());
			}
		} else {
			struct sockaddr_in rtpto;
			rtpto.sin_port = ntohs(port);
			rtpto.sin_family = AF_INET;
			rtpto.sin_addr.s_addr = inet_addr(get_peer_ip().c_str());
			pUdpSockRef->send("\xce\xfa\xed\xfe", 4,SOCKET_DEFAULE_FLAGS, (struct sockaddr *) &rtpto);
		}
	}

	if (uiTrackIndex < _aTrackInfo.size() - 1) {
		//需要继续发送SETUP命令
		sendSetup(uiTrackIndex + 1);
		return;
	}

	for (unsigned int i = 0; i < _aTrackInfo.size() && _eType != RTP_TCP; i++) {
		auto &pUdpSockRef = _apUdpSock[i];
		if(!pUdpSockRef){
			continue;
		}
		auto srcIP = inet_addr(get_peer_ip().data());
		weak_ptr<RtspPlayer> weakSelf = dynamic_pointer_cast<RtspPlayer>(shared_from_this());
		pUdpSockRef->setOnRead([srcIP,i,weakSelf](const Buffer::Ptr &buf, struct sockaddr *addr) {
			auto strongSelf=weakSelf.lock();
			if(!strongSelf) {
				return;
			}
			if(((struct sockaddr_in *)addr)->sin_addr.s_addr != srcIP) {
				WarnL << "收到请他地址的UDP数据:" << inet_ntoa(((struct sockaddr_in *) addr)->sin_addr);
				return;
			}
			strongSelf->handleOneRtp(i,(unsigned char *)buf->data(),buf->size());
		});
	}
	/////////////////////////心跳/////////////////////////////////
	weak_ptr<RtspPlayer> weakSelf = dynamic_pointer_cast<RtspPlayer>(shared_from_this());
	_pBeatTimer.reset(new Timer(5, [weakSelf](){
		auto strongSelf = weakSelf.lock();
		if (!strongSelf){
			return false;
		}
		return strongSelf->sendOptions();
	},getExecutor()));
	pause(false);
}

bool RtspPlayer::sendOptions() {
	_onHandshake = [](const Parser& parser){
//		DebugL << "options response";
		return true;
	};
	return sendRtspRequest("OPTIONS",_strContentBase);
}

bool RtspPlayer::sendDescribe() {
	//发送DESCRIBE命令后处理函数:handleResDESCRIBE
	_onHandshake = std::bind(&RtspPlayer::handleResDESCRIBE,this, placeholders::_1);
	StrCaseMap header;
	header["Accept"] = "application/sdp";
	return sendRtspRequest("DESCRIBE",_strUrl,header);
}


bool RtspPlayer::sendPause(bool bPause,uint32_t seekMS){
    if(!bPause){
        //修改时间轴
        int iTimeInc = seekMS - getProgressMilliSecond();
        for(unsigned int i = 0 ;i < _aTrackInfo.size() ;i++){
			_aiFistStamp[i] = _aiNowStamp[i] + iTimeInc;
            _aiNowStamp[i] = _aiFistStamp[i];
        }
        _iSeekTo = seekMS;
    }

	//开启或暂停rtsp
	_onHandshake = std::bind(&RtspPlayer::handleResPAUSE,this, placeholders::_1,bPause);

	StrCaseMap header;
	char buf[8];
	sprintf(buf,"%.2f",seekMS / 1000.0);
	header["Range"] = StrPrinter << "npt=" << buf << "-";
	return sendRtspRequest(bPause ? "PAUSE" : "PLAY",_strContentBase,header);
}
void RtspPlayer::pause(bool bPause) {
    sendPause(bPause, getProgressMilliSecond());
}

void RtspPlayer::handleResPAUSE(const Parser& parser, bool bPause) {
	if (parser.Url() != "200") {
		WarnL <<(bPause ? "Pause" : "Play") << " failed:" << parser.Url() << " " << parser.Tail() << endl;
		return;
	}
	if (!bPause) {
        //修正时间轴
        auto strRange = parser["Range"];
        if (strRange.size()) {
            auto strStart = FindField(strRange.data(), "npt=", "-");
            if (strStart == "now") {
                strStart = "0";
            }
            _iSeekTo = 1000 * atof(strStart.data());
            DebugL << "seekTo(ms):" << _iSeekTo ;
        }
        auto strRtpInfo =  parser["RTP-Info"];
        if (strRtpInfo.size()) {
            strRtpInfo.append(",");
            vector<string> vec = split(strRtpInfo, ",");
            for(auto &strTrack : vec){
                strTrack.append(";");
                auto strControlSuffix = strTrack.substr(1 + strTrack.rfind('/'),strTrack.find(';') - strTrack.rfind('/') - 1);
                auto strRtpTime = FindField(strTrack.data(), "rtptime=", ";");
                auto idx = getTrackIndexByControlSuffix(strControlSuffix);
                if(idx != -1){
                    _aiFistStamp[idx] = atoll(strRtpTime.data()) * 1000 / _aTrackInfo[idx]->_samplerate;
                    _aiNowStamp[idx] = _aiFistStamp[idx];
                    DebugL << "rtptime(ms):" << strControlSuffix <<" " << strRtpTime;
                }
            }
        }
		onPlayResult_l(SockException(Err_success, "rtsp play success"));
	} else {
		_pRtpTimer.reset();
	}
}

void RtspPlayer::onWholeRtspPacket(Parser &parser) {
    try {
		decltype(_onHandshake) fun;
		_onHandshake.swap(fun);
        if(fun){
            fun(parser);
        }
        parser.Clear();
    } catch (std::exception &err) {
        SockException ex(Err_other, err.what());
        onPlayResult_l(ex);
        onShutdown_l(ex);
        teardown();
    }
}

void RtspPlayer::onRtpPacket(const char *data, uint64_t len) {
    if(len > 1600){
        //没有大于MTU的包
        return;
    }
    int trackIdx = -1;
    uint8_t interleaved = data[1];
    if(interleaved %2 ==0){
        trackIdx = getTrackIndexByInterleaved(interleaved);
    }
    if (trackIdx != -1) {
        handleOneRtp(trackIdx, (unsigned char *)data + 4, len - 4);
    }
}

#   define AV_RB16(x)                           \
    ((((const uint8_t*)(x))[0] << 8) |          \
      ((const uint8_t*)(x))[1])


bool RtspPlayer::handleOneRtp(int iTrackidx, unsigned char *pucData, unsigned int uiLen) {
	auto &track = _aTrackInfo[iTrackidx];
	auto pt_ptr=_pktPool.obtain();
	auto &rtppt=*pt_ptr;
	rtppt.interleaved = track->_interleaved;
	rtppt.length = uiLen + 4;

	rtppt.mark = pucData[1] >> 7;
	rtppt.PT = pucData[1] & 0x7F;
	//序列号
	memcpy(&rtppt.sequence,pucData+2,2);//内存对齐
	rtppt.sequence = ntohs(rtppt.sequence);
	//时间戳
    memcpy(&rtppt.timeStamp, pucData+4, 4);//内存对齐
	//时间戳转换成毫秒
    rtppt.timeStamp = ntohl(rtppt.timeStamp) * 1000L / track->_samplerate;
	//ssrc
	memcpy(&rtppt.ssrc,pucData+8,4);//内存对齐
	rtppt.ssrc = ntohl(rtppt.ssrc);
	rtppt.type = track->_type;
	if (track->_ssrc == 0) {
		track->_ssrc = rtppt.ssrc;
		//保存SSRC
	} else if (track->_ssrc != rtppt.ssrc) {
		//ssrc错误
		WarnL << "ssrc错误";
		if (_aui32SsrcErrorCnt[iTrackidx]++ > 10) {
			track->_ssrc = rtppt.ssrc;
			WarnL << "ssrc更换!";
		}
		return false;
	}
	_aui32SsrcErrorCnt[iTrackidx] = 0;

	rtppt.payload[0] = '$';
	rtppt.payload[1] = rtppt.interleaved;
	rtppt.payload[2] = (uiLen & 0xFF00) >> 8;
	rtppt.payload[3] = (uiLen & 0x00FF);

	rtppt.offset 	= 16;
	int csrc     	= pucData[0] & 0x0f;
	int ext      	= pucData[0] & 0x10;
	rtppt.offset 	+= 4 * csrc;
	if (ext) {
		if(uiLen < rtppt.offset){
			return false;
		}
		/* calculate the header extension length (stored as number of 32-bit words) */
		ext = (AV_RB16(pucData + rtppt.offset - 2) + 1) << 2;
		rtppt.offset += ext;
	}

	memcpy(rtppt.payload + 4, pucData, uiLen);

	/////////////////////////////////RTP排序逻辑///////////////////////////////////
	if(rtppt.sequence != (uint16_t)(_aui16LastSeq[iTrackidx] + 1) && _aui16LastSeq[iTrackidx] != 0){
		//包乱序或丢包
		_aui64SeqOkCnt[iTrackidx] = 0;
		_abSortStarted[iTrackidx] = true;
		//WarnL << "包乱序或丢包:" << trackidx <<" " << rtppt.sequence << " " << _aui16LastSeq[trackidx];
	}else{
		//正确序列的包
		_aui64SeqOkCnt[iTrackidx]++;
	}
	_aui16LastSeq[iTrackidx] = rtppt.sequence;

	//开始排序缓存
	if (_abSortStarted[iTrackidx]) {
		_amapRtpSort[iTrackidx].emplace(rtppt.sequence, pt_ptr);
        GET_CONFIG_AND_REGISTER(uint32_t,clearCount,Rtp::kClearCount);
        GET_CONFIG_AND_REGISTER(uint32_t,maxRtpCount,Rtp::kMaxRtpCount);
		if (_aui64SeqOkCnt[iTrackidx] >= clearCount) {
			//网络环境改善，需要清空排序缓存
			_aui64SeqOkCnt[iTrackidx] = 0;
			_abSortStarted[iTrackidx] = false;
			while (_amapRtpSort[iTrackidx].size()) {
				POP_HEAD(iTrackidx)
			}
		} else if (_amapRtpSort[iTrackidx].size() >= maxRtpCount) {
			//排序缓存溢出
			POP_HEAD(iTrackidx)
		}
	}else{
		//正确序列
		onRecvRTP_l(pt_ptr, iTrackidx);
	}
	//////////////////////////////////////////////////////////////////////////////////
	return true;
}
void RtspPlayer::onRecvRTP_l(const RtpPacket::Ptr &rtppt, int trackidx){
	//统计丢包率
	if (_aui16FirstSeq[trackidx] == 0 || rtppt->sequence < _aui16FirstSeq[trackidx]) {
		_aui16FirstSeq[trackidx] = rtppt->sequence;
		_aui64RtpRecv[trackidx] = 0;
	}
	_aui64RtpRecv[trackidx] ++;
	_aui16NowSeq[trackidx] = rtppt->sequence;
	_aiNowStamp[trackidx] = rtppt->timeStamp;
	if( _aiFistStamp[trackidx] == 0){
		_aiFistStamp[trackidx] = _aiNowStamp[trackidx];
	}

    rtppt->timeStamp -= _aiFistStamp[trackidx];
	onRecvRTP_l(rtppt,_aTrackInfo[trackidx]);
}
float RtspPlayer::getPacketLossRate(TrackType type) const{
	int iTrackIdx = getTrackIndexByTrackType(type);
	if(iTrackIdx == -1){
		uint64_t totalRecv = 0;
		uint64_t totalSend = 0;
		for (unsigned int i = 0; i < _aTrackInfo.size(); i++) {
			totalRecv += _aui64RtpRecv[i];
			totalSend += (_aui16NowSeq[i] - _aui16FirstSeq[i] + 1);
		}
		if(totalSend == 0){
			return 0;
		}
		return 1.0 - (double)totalRecv / totalSend;
	}


	if(_aui16NowSeq[iTrackIdx] - _aui16FirstSeq[iTrackIdx] + 1 == 0){
		return 0;
	}
	return 1.0 - (double)_aui64RtpRecv[iTrackIdx] / (_aui16NowSeq[iTrackIdx] - _aui16FirstSeq[iTrackIdx] + 1);
}

uint32_t RtspPlayer::getProgressMilliSecond() const{
	uint32_t iTime[2] = {0,0};
    for(unsigned int i = 0 ;i < _aTrackInfo.size() ;i++){
		iTime[i] = _aiNowStamp[i] - _aiFistStamp[i];
    }
    return _iSeekTo + MAX(iTime[0],iTime[1]);
}
void RtspPlayer::seekToMilliSecond(uint32_t ms) {
    sendPause(false,ms);
}

bool RtspPlayer::sendRtspRequest(const string &cmd, const string &url,const StrCaseMap &header_const) {
	auto header = header_const;
	header.emplace("CSeq",StrPrinter << _uiCseq++);
	if(!_strSession.empty()){
		header.emplace("Session",_strSession);
	}

	if(!(*this)[kRtspRealm].empty() && !(*this)[PlayerBase::kRtspUser].empty()){
		if(!(*this)[kRtspMd5Nonce].empty()){
			//MD5认证
			/*
			response计算方法如下：
			RTSP客户端应该使用username + password并计算response如下:
			(1)当password为MD5编码,则
				response = md5( password:nonce:md5(public_method:url)  );
			(2)当password为ANSI字符串,则
				response= md5( md5(username:realm:password):nonce:md5(public_method:url) );
			 */
			string encrypted_pwd = (*this)[PlayerBase::kRtspPwd];
			if(!(*this)[PlayerBase::kRtspPwdIsMD5].as<bool>()){
				encrypted_pwd = MD5((*this)[PlayerBase::kRtspUser]+ ":" + (*this)[kRtspRealm] + ":" + encrypted_pwd).hexdigest();
			}
			auto response = MD5( encrypted_pwd + ":" + (*this)[kRtspMd5Nonce]  + ":" + MD5(cmd + ":" + url).hexdigest()).hexdigest();
			_StrPrinter printer;
			printer << "Digest ";
			printer << "username=\"" << (*this)[PlayerBase::kRtspUser] << "\", ";
			printer << "realm=\"" << (*this)[kRtspRealm] << "\", ";
			printer << "nonce=\"" << (*this)[kRtspMd5Nonce] << "\", ";
			printer << "uri=\"" << url << "\", ";
			printer << "response=\"" << response << "\"";
			header.emplace("Authorization",printer);
		}else if(!(*this)[PlayerBase::kRtspPwdIsMD5].as<bool>()){
			//base64认证
			string authStr = StrPrinter << (*this)[PlayerBase::kRtspUser] << ":" << (*this)[PlayerBase::kRtspPwd];
			char authStrBase64[1024] = {0};
			av_base64_encode(authStrBase64,sizeof(authStrBase64),(uint8_t *)authStr.data(),authStr.size());
			header.emplace("Authorization",StrPrinter << "Basic " << authStrBase64 );
		}
	}

	_StrPrinter printer;
	printer << cmd << " " << url << " RTSP/1.0\r\n";
	for (auto &pr : header){
		printer << pr.first << ": " << pr.second << "\r\n";
	}
	return send(printer << "\r\n") > 0;
}


void RtspPlayer::onShutdown_l(const SockException &ex) {
	WarnL << ex.getErrCode() << " " << ex.what();
	_pPlayTimer.reset();
	_pRtpTimer.reset();
	_pBeatTimer.reset();
	onShutdown(ex);
}
void RtspPlayer::onRecvRTP_l(const RtpPacket::Ptr &pRtppt, const SdpTrack::Ptr &track) {
	_rtpTicker.resetTime();
	onRecvRTP(pRtppt,track);
}
void RtspPlayer::onPlayResult_l(const SockException &ex) {
	WarnL << ex.getErrCode() << " " << ex.what();
	_pPlayTimer.reset();
	_pRtpTimer.reset();
	if (!ex) {
		_rtpTicker.resetTime();
		weak_ptr<RtspPlayer> weakSelf = dynamic_pointer_cast<RtspPlayer>(shared_from_this());
		_pRtpTimer.reset( new Timer(5, [weakSelf]() {
			auto strongSelf=weakSelf.lock();
			if(!strongSelf) {
				return false;
			}
			if(strongSelf->_rtpTicker.elapsedTime()>10000) {
				//recv rtp timeout!
				strongSelf->onShutdown_l(SockException(Err_timeout,"recv rtp timeout"));
				strongSelf->teardown();
				return false;
			}
			return true;
		},getExecutor()));
	}
	onPlayResult(ex);
}

int RtspPlayer::getTrackIndexByControlSuffix(const string &controlSuffix) const{
	for (unsigned int i = 0; i < _aTrackInfo.size(); i++) {
		auto pos = _aTrackInfo[i]->_control_surffix.find(controlSuffix);
		if (pos == 0) {
			return i;
		}
	}
	return -1;
}
int RtspPlayer::getTrackIndexByInterleaved(int interleaved) const{
	for (unsigned int i = 0; i < _aTrackInfo.size(); i++) {
		if (_aTrackInfo[i]->_interleaved == interleaved) {
			return i;
		}
	}
	return -1;
}

int RtspPlayer::getTrackIndexByTrackType(TrackType trackType) const {
	for (unsigned int i = 0; i < _aTrackInfo.size(); i++) {
		if (_aTrackInfo[i]->_type == trackType) {
			return i;
		}
	}
	return -1;
}

} /* namespace mediakit */


