/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "config.h"

#include "net/DelaySocket.hpp"

class Delayer;

// FIXME: TerminatingPoll ?
static SocketPoll DelayPoll("delay_poll");

/// Reads from fd, delays that and then writes to _dest.
class DelaySocket : public Socket {
    int _delayMs;
    bool _closed;
    bool _stopPoll;
    bool _waitForWrite;
    std::shared_ptr<DelaySocket> _dest;

    const size_t WindowSize = 64 * 1024;

    /// queued up data - sent to us by our opposite twin.
    struct WriteChunk {
        std::chrono::steady_clock::time_point _sendTime;
        std::vector<char> _data;
        WriteChunk(int delayMs)
        {
            _sendTime = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(delayMs);
        }
        bool isError() { return _data.size() == 0; }
    private:
        WriteChunk();
    };

    std::vector<std::shared_ptr<WriteChunk>> _chunks;
public:
    DelaySocket(int delayMs, int fd) :
        Socket (fd), _delayMs(delayMs), _closed(false),
        _stopPoll(false), _waitForWrite(false)
	{
//        setSocketBufferSize(Socket::DefaultSendBufferSize);
	}
    void setDestination(const std::shared_ptr<DelaySocket> &dest)
    {
        _dest = dest;
    }

    void dumpState(std::ostream& os) override
    {
        os << "\tfd: " << getFD()
           << "\n\tqueue: " << _chunks.size() << "\n";
        auto now = std::chrono::steady_clock::now();
        for (auto &chunk : _chunks)
        {
            os << "\t\tin: " <<
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    chunk->_sendTime - now).count() << "ms - "
               << chunk->_data.size() << "bytes\n";
        }
    }

    // FIXME - really need to propagate 'noDelay' etc.
    // have a debug only lookup of delayed sockets for this case ?

    int getPollEvents(std::chrono::steady_clock::time_point now,
                      int &timeoutMaxMs) override
    {
        if (_chunks.size() > 0)
        {
            int remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                (*_chunks.begin())->_sendTime - now).count();
            if (remainingMs < timeoutMaxMs)
                std::cerr << "#" << getFD() << " reset timeout max to " << remainingMs
                          << "ms from " << timeoutMaxMs << "ms\n";
            timeoutMaxMs = std::min(timeoutMaxMs, remainingMs);
        }

        if (_chunks.size() > 0 &&
            now > (*_chunks.begin())->_sendTime)
            return POLLIN | POLLOUT;
        else
            return POLLIN;
    }

    void pushCloseChunk(bool bErrorSocket)
    {
        // socket in error state ? don't keep polling it.
        _stopPoll |= bErrorSocket;
        _chunks.push_back(std::make_shared<WriteChunk>(_delayMs));
    }

    HandleResult handlePoll(std::chrono::steady_clock::time_point now, int events) override
    {
        if (events & POLLIN)
        {
            auto chunk = std::make_shared<WriteChunk>(_delayMs);

            char buf[64 * 1024];
            ssize_t len;
            size_t toRead = sizeof(buf); //std::min(sizeof(buf), WindowSize - _chunksSize);
            do {
                len = ::read(getFD(), buf, toRead);
            } while (len < 0 && errno == EINTR);

            if (len == 0)
            { // EOF.
                if (_dest) // FIXME: cut and paste ...
                {
                    _dest->pushCloseChunk(false);
                    _dest = nullptr;
                }
                std::cerr << "EOF on input\n";
                shutdown();
                return HandleResult::SOCKET_CLOSED;
            }

            if (len >= 0)
            {
                std::cerr << "#" << getFD() << " read " << len
                      << " to queue: " << _chunks.size() << "\n";
                chunk->_data.insert(chunk->_data.end(), &buf[0], &buf[len]);
                if (_dest)
                    _dest->_chunks.push_back(chunk);
                else
                    std::cerr << "no destination for data\n";
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                std::cerr << "#" << getFD() << " error : " << errno << " " << strerror(errno) << "\n";
                pushCloseChunk(true);
            }
        }

        // Write if we have delayed enough.
        if (_chunks.size() > 0)
        {
            std::shared_ptr<WriteChunk> chunk = *_chunks.begin();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - chunk->_sendTime).count() >= 0)
            {
                if (chunk->_data.size() == 0)
                { // delayed error or close
                    std::cerr << "#" << getFD() << " handling delayed close\n";
                    _closed = true;
                    _dest = nullptr;
                    shutdown();
                    return HandleResult::SOCKET_CLOSED;
                }

                ssize_t len;
                do {
                    len = ::write(getFD(), &chunk->_data[0], chunk->_data.size());
                } while (len < 0 && errno == EINTR);

                if (len < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        std::cerr << "#" << getFD() << " full - waiting for write\n";
                    }
                    else
                    {
                        std::cerr << "#" << getFD() << " failed onwards write " << len << "bytes of "
                                  << chunk->_data.size()
                                  << " queue: " << _chunks.size() << " error " << strerror(errno) << "\n";
                        // URGH - cut and paste ...
                        _closed = true;
                        _dest = nullptr;
                        // FIXME: tell dest we're dead ... [!] ...
                        shutdown();
                        return HandleResult::SOCKET_CLOSED;
                    }
                }
                else
                {
                    std::cerr << "#" << getFD() << " written onwards " << len << "bytes of "
                              << chunk->_data.size()
                              << " queue: " << _chunks.size() << "\n";
                    if (len > 0)
                    {
                        chunk->_data.erase(chunk->_data.begin(), chunk->_data.begin() + len);
                    }

                    if (chunk->_data.size() == 0)
                        _chunks.erase(_chunks.begin(), _chunks.begin() + 1);
                }
            }
        }

        // FIXME: ideally we could avoid polling & delay _closed state etc.
        if (events & (POLLERR | POLLHUP | POLLNVAL))
        {
            std::cerr << "#" << getFD() << " error events: " << events << "\n";
            pushCloseChunk(true);
        }
        return HandleResult::CONTINUE;
    }
};

/// Delayer:
///
/// Some terminology:
///    physical socket (DelaySocket's own fd) - what we accepted.
///    internalFd - the internal side of the socket-pair
///    delayFd - what we hand on to our un-suspecting wrapped socket
///              which looks like an external socket - but delayed.
namespace Delay {
    int create(int delayMs, int physicalFd)
    {
        int pair[2];
        int rc = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair);
        assert (rc == 0);
        int internalFd = pair[0];
        int delayFd = pair[1];

        auto physical = std::make_shared<DelaySocket>(delayMs, physicalFd);
        auto internal = std::make_shared<DelaySocket>(delayMs, internalFd);
        physical->setDestination(internal);
        internal->setDestination(physical);

        DelayPoll.startThread();
        DelayPoll.insertNewSocket(physical);
        DelayPoll.insertNewSocket(internal);

        return delayFd;
    }
    void dumpState(std::ostream &os)
    {
        if (DelayPoll.isAlive())
        {
            os << "Delay poll:\n";
            DelayPoll.dumpState(os);
        }
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
