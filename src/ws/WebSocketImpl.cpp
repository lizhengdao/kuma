/* Copyright (c) 2014, Fengping Bao <jamol@live.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "WebSocketImpl.h"
#include "util/kmtrace.h"
#include "util/util.h"

#include <sstream>

KUMA_NS_BEGIN

//////////////////////////////////////////////////////////////////////////
WebSocketImpl::WebSocketImpl(EventLoopImpl* loop)
: state_(STATE_IDLE)
, init_data_(nullptr)
, init_len_(0)
, send_offset_(0)
, tcp_socket_(loop)
, is_server_(false)
, body_bytes_sent_(0)
, destroy_flag_ptr_(nullptr)
{
    
}

WebSocketImpl::~WebSocketImpl()
{
    if(destroy_flag_ptr_) {
        *destroy_flag_ptr_ = true;
    }
    if(init_data_) {
        delete [] init_data_;
        init_data_ = nullptr;
        init_len_ = 0;
    }
}

const char* WebSocketImpl::getObjKey()
{
    return "WebSocket";
}

void WebSocketImpl::cleanup()
{
    tcp_socket_.close();
    send_buffer_.clear();
    send_offset_ = 0;
}

void WebSocketImpl::setProtocol(const std::string& proto)
{
    proto_ = proto;
}

void WebSocketImpl::setOrigin(const std::string& origin)
{
    origin_ = origin;
}

int WebSocketImpl::connect(const std::string& ws_url, EventCallback& cb)
{
    if(getState() != STATE_IDLE) {
        KUMA_ERRXTRACE("connect, invalid state, state="<<getState());
        return KUMA_ERROR_INVALID_STATE;
    }
    cb_connect_ = cb;
    return connect_i(ws_url);
}

int WebSocketImpl::connect(const std::string& ws_url, EventCallback&& cb)
{
    if(getState() != STATE_IDLE) {
        KUMA_ERRXTRACE("connect, invalid state, state="<<getState());
        return KUMA_ERROR_INVALID_STATE;
    }
    cb_connect_ = std::move(cb);
    return connect_i(ws_url);
}

int WebSocketImpl::connect_i(const std::string& ws_url)
{
    if(!uri_.parse(ws_url)) {
        return false;
    }
    tcp_socket_.setReadCallback([this] (int err) { onReceive(err); });
    tcp_socket_.setWriteCallback([this] (int err) { onSend(err); });
    tcp_socket_.setErrorCallback([this] (int err) { onClose(err); });
    setState(STATE_CONNECTING);
    std::string str_port = uri_.getPort();
    uint16_t port = 80;
    uint32_t flag = 0;
    if(is_equal("wss", uri_.getScheme())) {
        port = 443;
        flag = FLAG_HAS_SSL;
    }
    if(!str_port.empty()) {
        port = atoi(str_port.c_str());
    }
    return tcp_socket_.connect(uri_.getHost().c_str(), port, [this] (int err) { onConnect(err); }, flag);
}

int WebSocketImpl::attachFd(SOCKET_FD fd, uint32_t flags, const uint8_t* init_data, uint32_t init_len)
{
    is_server_ = true;
    if(init_data && init_len > 0) {
        init_data = new uint8_t(init_len);
        memcpy(init_data_, init_data, init_len);
        init_len_ = init_len;
    }
    ws_handler_.setDataCallback([this] (uint8_t* data, uint32_t len) { onWsData(data, len); });
    ws_handler_.setHandshakeCallback([this] (int err) { onWsHandshake(err); });
    tcp_socket_.setReadCallback([this] (int err) { onReceive(err); });
    tcp_socket_.setWriteCallback([this] (int err) { onSend(err); });
    tcp_socket_.setErrorCallback([this] (int err) { onClose(err); });
    setState(STATE_HANDSHAKE);
    return tcp_socket_.attachFd(fd, flags);
}

int WebSocketImpl::attachSocket(TcpSocketImpl&& tcp, HttpParserImpl&& parser)
{
    is_server_ = true;
    ws_handler_.setDataCallback([this] (uint8_t* data, uint32_t len) { onWsData(data, len); });
    ws_handler_.setHandshakeCallback([this] (int err) { onWsHandshake(err); });
    tcp_socket_.setReadCallback([this] (int err) { onReceive(err); });
    tcp_socket_.setWriteCallback([this] (int err) { onSend(err); });
    tcp_socket_.setErrorCallback([this] (int err) { onClose(err); });
    setState(STATE_HANDSHAKE);

#ifdef KUMA_HAS_OPENSSL
    SOCKET_FD fd;
    SSL* ssl = nullptr;
    uint32_t flags = tcp.getFlags();
    int ret = tcp.detachFd(fd, ssl);
    ret = tcp_socket_.attachFd(fd, ssl, flags);
#else
    SOCKET_FD fd;
    uint32_t flags = tcp.getFlags();
    int ret = tcp.detachFd(fd);
    ret = tcp_socket_.attachFd(fd, flags);
#endif
    
    ws_handler_.setHttpParser(std::move(parser));
    return ret;
}

int WebSocketImpl::send(const uint8_t* data, uint32_t len)
{
    if(getState() != STATE_OPEN) {
        return -1;
    }
    if(!send_buffer_.empty()) {
        return 0;
    }
    uint8_t hdr[10];
    WSHandler::WSOpcode opcode = WSHandler::WSOpcode::WS_OPCODE_BINARY;
    if(ws_handler_.getOpcode() == WSHandler::WSOpcode::WS_OPCODE_TEXT) {
        opcode = WSHandler::WSOpcode::WS_OPCODE_TEXT;
    }
    int hdr_len = ws_handler_.encodeFrameHeader(opcode, len, hdr);
#if 0
    send_buffer_.resize(hdr_len + len);
    memcpy(&send_buffer_[0], hdr, hdr_len);
    memcpy(&send_buffer_[0] + hdr_len, data, len);
    int ret = tcp_socket_.send(&send_buffer_[0], hdr_len + len);
    if(ret < 0) {
        return ret;
    } else if(ret < hdr_len + len) {
        send_offset_ = ret;
    } else {
        send_buffer_.clear();
        send_offset_ = 0;
    }
#else
    iovec iovs[2];
    iovs[0].iov_base = (char*)hdr;
    iovs[0].iov_len = hdr_len;
    iovs[1].iov_base = (char*)data;
    iovs[1].iov_len = len;
    auto total_len = iovs[0].iov_len + iovs[1].iov_len;
    int ret = tcp_socket_.send(iovs, 2);
    if(ret < 0) {
        return ret;
    } else if(ret < total_len) {
        send_buffer_.reserve(total_len - ret);
        send_offset_ = 0;
        for (auto &iov : iovs) {
            const uint8_t* first = ((uint8_t*)iov.iov_base) + ret;
            const uint8_t* last = ((uint8_t*)iov.iov_base) + iov.iov_len;
            if(first < last) {
                send_buffer_.insert(send_buffer_.end(), first, last);
                ret = 0;
            } else {
                ret -= iov.iov_len;
            }
        }
    }
#endif
    return len;
}

int WebSocketImpl::close()
{
    KUMA_INFOXTRACE("close");
    cleanup();
    setState(STATE_CLOSED);
    return KUMA_ERROR_NOERR;
}

void WebSocketImpl::onConnect(int err)
{
    if(err != KUMA_ERROR_NOERR) {
        if(cb_error_) cb_error_(err);
        return ;
    }
    ws_handler_.setDataCallback([this] (uint8_t* data, uint32_t len) { onWsData(data, len); });
    ws_handler_.setHandshakeCallback([this] (int err) { onWsHandshake(err); });
    body_bytes_sent_ = 0;
    std::string str(ws_handler_.buildRequest(uri_.getPath(), uri_.getHost(), proto_, origin_));
    send_buffer_.clear();
    send_offset_ = 0;
    send_buffer_.reserve(str.length());
    send_buffer_.insert(send_buffer_.end(), str.begin(), str.end());
    setState(STATE_HANDSHAKE);
    int ret = tcp_socket_.send(&send_buffer_[0], (uint32_t)send_buffer_.size());
    if(ret < 0) {
        cleanup();
        setState(STATE_CLOSED);
        if(cb_error_) cb_error_(KUMA_ERROR_SOCKERR);
        return;
    } else {
        send_offset_ += ret;
        if(send_offset_ == send_buffer_.size()) {
            send_offset_ = 0;
            send_buffer_.clear();
        }
    }
}

void WebSocketImpl::onSend(int err)
{
    if(!send_buffer_.empty() && send_offset_ < send_buffer_.size()) {
        int ret = tcp_socket_.send(&send_buffer_[0] + send_offset_, (uint32_t)send_buffer_.size() - send_offset_);
        if(ret < 0) {
            cleanup();
            setState(STATE_CLOSED);
            if(cb_error_) cb_error_(KUMA_ERROR_SOCKERR);
            return;
        } else {
            send_offset_ += ret;
            if(send_offset_ == send_buffer_.size()) {
                send_offset_ = 0;
                send_buffer_.clear();
                if(is_server_ && getState() == STATE_HANDSHAKE) {
                    onStateOpen(); // response is sent out
                }
            }
        }
    }
    if(send_buffer_.empty() && cb_write_) {
        cb_write_(0);
    }
}

void WebSocketImpl::onReceive(int err)
{
    if(init_data_ && init_len_ > 0) {
        bool destroyed = false;
        KUMA_ASSERT(nullptr == destroy_flag_ptr_);
        destroy_flag_ptr_ = &destroyed;
        WSHandler::WSError err = ws_handler_.handleData(init_data_, init_len_);
        if(destroyed) {
            return;
        }
        destroy_flag_ptr_ = nullptr;
        delete [] init_data_;
        init_data_ = nullptr;
        init_len_ = 0;
        if(getState() == STATE_ERROR || getState() == STATE_CLOSED) {
            return;
        }
        if(err != WSHandler::WSError::WS_ERROR_NOERR &&
           err != WSHandler::WSError::WS_ERROR_NEED_MORE_DATA) {
            cleanup();
            setState(STATE_CLOSED);
            if(cb_error_) cb_error_(KUMA_ERROR_FAILED);
            return;
        }
    }
    uint8_t buf[128*1024];
    do {
        int ret = tcp_socket_.receive(buf, sizeof(buf));
        if(ret < 0) {
            cleanup();
            setState(STATE_CLOSED);
            if(cb_error_) cb_error_(KUMA_ERROR_SOCKERR);
        } else if(0 == ret) {
            break;
        } else if(getState() == STATE_HANDSHAKE || getState() == STATE_OPEN) {
            bool destroyed = false;
            KUMA_ASSERT(nullptr == destroy_flag_ptr_);
            destroy_flag_ptr_ = &destroyed;
            WSHandler::WSError err = ws_handler_.handleData(buf, ret);
            if(destroyed) {
                return;
            }
            destroy_flag_ptr_ = nullptr;
            if(getState() == STATE_ERROR || getState() == STATE_CLOSED) {
                break;
            }
            if(err != WSHandler::WSError::WS_ERROR_NOERR &&
               err != WSHandler::WSError::WS_ERROR_NEED_MORE_DATA) {
                cleanup();
                setState(STATE_CLOSED);
                if(cb_error_) cb_error_(KUMA_ERROR_FAILED);
                return;
            }
        } else {
            KUMA_WARNXTRACE("onReceive, invalid state: "<<getState());
        }
    } while(true);
}

void WebSocketImpl::onClose(int err)
{
    KUMA_INFOXTRACE("onClose, err="<<err);
    cleanup();
    setState(STATE_CLOSED);
    if(cb_error_) cb_error_(KUMA_ERROR_SOCKERR);
}

void WebSocketImpl::sendWsResponse()
{
    std::string str(ws_handler_.buildResponse());
    send_buffer_.clear();
    send_offset_ = 0;
    send_buffer_.reserve(str.length());
    send_buffer_.insert(send_buffer_.end(), str.begin(), str.end());
    int ret = tcp_socket_.send(&send_buffer_[0], (uint32_t)send_buffer_.size());
    if(ret < 0) {
        cleanup();
        setState(STATE_CLOSED);
        if(cb_error_) cb_error_(KUMA_ERROR_SOCKERR);
        return;
    } else {
        send_offset_ += ret;
        if(send_offset_ == send_buffer_.size()) {
            send_offset_ = 0;
            send_buffer_.clear();
            onStateOpen();
        }
    }
}

void WebSocketImpl::onStateOpen()
{
    KUMA_INFOXTRACE("onStateOpen");
    setState(STATE_OPEN);
    if(is_server_) {
        if(cb_write_) cb_write_(0);
    } else {
        if(cb_connect_) cb_connect_(0);
    }
}

void WebSocketImpl::onWsData(uint8_t* data, uint32_t len)
{
    if(cb_data_) cb_data_(data, len);
}

void WebSocketImpl::onWsHandshake(int err)
{
    if(0 == err) {
        if(is_server_) {
            sendWsResponse();
        } else {
            onStateOpen();
        }
    } else {
        setState(STATE_ERROR);
        if(cb_error_) cb_error_(KUMA_ERROR_FAILED);
    }
}

KUMA_NS_END
