#include <thread>

#include <Net/Socket.h>
#include <Net/SocketException.h>
#include <Core/Global.h>
#include <Common/Util/ThreadUtil.h>
#include <Common/Util/HexUtil.h>
#include <Common/Logger.h>

static unsigned long DEFAULT_TIMEOUT = 1 * 1000;
static constexpr size_t ASYNC_WRITE_LOG_THRESHOLD_BYTES = 64 * 1024;
static constexpr uint64_t ASYNC_WRITE_SLOW_LOG_THRESHOLD_MS = 5000;
static constexpr size_t ASYNC_WRITE_QUEUE_DEPTH_LOG_THRESHOLD = 8;

static uint64_t ElapsedMillis(const std::chrono::steady_clock::time_point& start)
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

#ifndef _WIN32
#define SOCKET_ERROR -1
#endif

Socket::Socket(
    SocketAddress socket_address,
    const std::shared_ptr<asio::io_context>& pContext,
    const std::shared_ptr<asio::ip::tcp::socket>& pSocket)
    : m_pSocket(pSocket),
    m_pContext(pContext),
    m_address(std::move(socket_address)),
    m_socketOpen(false),
    m_failed(false),
    m_blocking(true),
    m_receiveBufferSize(0),
    m_receiveTimeout(DEFAULT_TIMEOUT),
    m_sendTimeout(DEFAULT_TIMEOUT)
{

}

Socket::~Socket()
{
    CloseSocket();

    std::unique_lock<std::mutex> queueLock(m_writeQueueMutex);
    std::unique_lock<std::shared_mutex> socketLock(m_socketMutex);
    m_pSocket.reset();
    m_pContext.reset();
}

bool Socket::CloseSocket()
{
    std::unique_lock<std::shared_mutex> socketLock(m_socketMutex);
    if (!m_socketOpen) {
        return true;
    }

    m_socketOpen = false;

    asio::error_code error;
    m_pSocket->shutdown(asio::socket_base::shutdown_both, error);
    m_pSocket->close(error);

    return !error;
}

bool Socket::IsActive() const
{
    if (m_socketOpen && !m_errorCode) {
        return true;
    }

    if (m_errorCode.value() == EAGAIN || m_errorCode.value() == EWOULDBLOCK) {
        return true;
    }

    if (m_errorCode) {
        LOG_INFO_F("Connection with {} not active. Error: {}", m_address, m_errorCode.message());
    } else {
        LOG_INFO_F("Connection with {} not active.", m_address);
    }

    return false;
}

bool Socket::SetDefaultOptions()
{
    asio::error_code ec;
    asio::socket_base::receive_buffer_size option(32 * 1024);
    m_pSocket->set_option(option, ec);
    if (ec) { return false; }
    asio::ip::tcp::no_delay no_delay_option(true);
    m_pSocket->set_option(no_delay_option, ec);
    if (ec) { return false; }

#ifdef _WIN32
    if (setsockopt(m_pSocket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, (char*)&DEFAULT_TIMEOUT, sizeof(DEFAULT_TIMEOUT)) == SOCKET_ERROR) {
        return false;
    }

    if (setsockopt(m_pSocket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, (char*)&DEFAULT_TIMEOUT, sizeof(DEFAULT_TIMEOUT)) == SOCKET_ERROR) {
        return false;
    }
#endif

    return true;
}

bool Socket::SetReceiveTimeout(const unsigned long milliseconds)
{
#ifdef _WIN32
    const int result = setsockopt(m_pSocket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, (char*)&milliseconds, sizeof(milliseconds));
#else
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = milliseconds * 1000;
    const int result = setsockopt(m_pSocket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
#endif
    if (result == 0) {
        m_receiveTimeout = milliseconds;
    } else {
        return false;
    }

    return true;
}

bool Socket::SetReceiveBufferSize(const int bufferSize)
{
    const int socketRcvBuff = bufferSize;
    const int result = setsockopt(m_pSocket->native_handle(), SOL_SOCKET, SO_RCVBUF, (const char*)&socketRcvBuff, sizeof(int));
    if (result == 0) {
        m_receiveBufferSize = bufferSize;
    } else {
        return false;
    }

    return true;
}

bool Socket::SetSendTimeout(const unsigned long milliseconds)
{
#ifdef _WIN32
    const int result = setsockopt(m_pSocket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, (char*)&milliseconds, sizeof(milliseconds));
#else
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = milliseconds * 1000;
    const int result = setsockopt(m_pSocket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
#endif

    if (result == 0) {
        m_sendTimeout = milliseconds;
        return true;
    } else {
        return false;
    }
}

bool Socket::SetBlocking(const bool blocking)
{
    if (m_blocking != blocking) {
#ifdef _WIN32
        unsigned long blockingValue = (blocking ? 0 : 1);
        const int result = ioctlsocket(m_pSocket->native_handle(), FIONBIO, &blockingValue);
        if (result == 0) {
            m_blocking = blocking;
        } else {
            int error = 0;
            int size = sizeof(error);
            getsockopt(m_pSocket->native_handle(), SOL_SOCKET, SO_ERROR, (char*)&error, &size);
            WSASetLastError(error);
            ThrowSocketException(m_errorCode);
        }
#else
        m_blocking = blocking;
#endif
    }

    return m_blocking == blocking;
}

bool Socket::SendSync(const std::vector<uint8_t>& message, const bool incrementCount)
{
    if (incrementCount) {
        m_rateCounter.AddMessageSent();
    }

    const size_t bytesWritten = asio::write(*m_pSocket, asio::buffer(message.data(), message.size()), m_errorCode);
    if (m_errorCode && m_errorCode.value() != EAGAIN && m_errorCode.value() != EWOULDBLOCK) {
        ThrowSocketException(m_errorCode);
    }

    return bytesWritten == message.size();
}

void Socket::SendAsync(const std::vector<uint8_t>& message, const uint8_t priority)
{
    std::unique_lock<std::mutex> queueLock(m_writeQueueMutex);
    const bool first_in_queue = m_writeQueue.empty();
    m_writeQueueBytes += message.size();
    QueuedWrite queuedWrite{
        message,
        std::chrono::steady_clock::now(),
        {},
        priority };

    if (!first_in_queue && priority > 0) {
        auto insertAt = m_writeQueue.begin() + 1; // The front write may already be in progress.
        while (insertAt != m_writeQueue.end() && insertAt->priority >= priority) {
            ++insertAt;
        }
        m_writeQueue.insert(insertAt, std::move(queuedWrite));
    } else {
        m_writeQueue.push_back(std::move(queuedWrite));
    }

    if (message.size() >= ASYNC_WRITE_LOG_THRESHOLD_BYTES) {
        LOG_TRACE(StringUtil::Format(
            "Socket queued async write to {}: bytes={}, priority={}, queue_depth={}, queue_bytes={}",
            Format(),
            message.size(),
            priority,
            m_writeQueue.size(),
            m_writeQueueBytes));
    }

    if (!first_in_queue) {
        // There is already an async_write in process.
        // It will send this message when it completes.
        return;
    }

    StartAsyncWriteLocked();
}

void Socket::StartAsyncWriteLocked()
{
    // The async buffer must reference the queued message, not a stack-local copy.
    // m_writeQueueMutex is held by the caller until async_write has captured it.
    if (m_writeQueue.empty()) {
        return;
    }

    QueuedWrite& queuedWrite = m_writeQueue.front();
    queuedWrite.writeStartedAt = std::chrono::steady_clock::now();

    std::shared_lock<std::shared_mutex> socketLock(m_socketMutex);
    if (m_socketOpen) {
        if (queuedWrite.bytes.size() >= ASYNC_WRITE_LOG_THRESHOLD_BYTES) {
            LOG_TRACE(StringUtil::Format(
                "Socket starting async write to {}: bytes={}, priority={}, queued_ms={}, queue_depth={}, queue_bytes={}",
                Format(),
                queuedWrite.bytes.size(),
                queuedWrite.priority,
                ElapsedMillis(queuedWrite.enqueuedAt),
                m_writeQueue.size(),
                m_writeQueueBytes));
        }

        asio::async_write(
            *m_pSocket,
            asio::buffer(queuedWrite.bytes.data(), queuedWrite.bytes.size()),
            std::bind(&Socket::HandleSent, shared_from_this(), std::placeholders::_1, std::placeholders::_2)
        );
    }
}

void Socket::HandleSent(const asio::error_code& ec, size_t bytes_transferred)
{
    m_rateCounter.AddMessageSent();

    std::unique_lock<std::mutex> queueLock(m_writeQueueMutex);
    size_t messageSize = 0;
    uint64_t queuedMs = 0;
    uint64_t writeMs = 0;
    uint8_t priority = 0;
    if (!m_writeQueue.empty()) {
        const QueuedWrite& queuedWrite = m_writeQueue.front();
        messageSize = queuedWrite.bytes.size();
        priority = queuedWrite.priority;
        queuedMs = ElapsedMillis(queuedWrite.enqueuedAt);
        writeMs = queuedWrite.writeStartedAt.time_since_epoch().count() == 0
            ? 0
            : ElapsedMillis(queuedWrite.writeStartedAt);
        if (m_writeQueueBytes >= messageSize) {
            m_writeQueueBytes -= messageSize;
        } else {
            m_writeQueueBytes = 0;
        }
        m_writeQueue.pop_front();
    }
    const size_t queueDepthAfterPop = m_writeQueue.size();
    const size_t queueBytesAfterPop = m_writeQueueBytes;

    if (ec) {
        LOG_INFO(StringUtil::Format(
            "Failed to send message to {}: {} (bytes={}, priority={}, transferred={}, queued_ms={}, write_ms={}, queue_depth={}, queue_bytes={})",
            Format(),
            ec.message(),
            messageSize,
            priority,
            bytes_transferred,
            queuedMs,
            writeMs,
            queueDepthAfterPop,
            queueBytesAfterPop));
    } else if (!m_writeQueue.empty()) {
        if (messageSize >= ASYNC_WRITE_LOG_THRESHOLD_BYTES) {
            const bool slowOrBacklogged = queuedMs >= ASYNC_WRITE_SLOW_LOG_THRESHOLD_MS
                || writeMs >= ASYNC_WRITE_SLOW_LOG_THRESHOLD_MS
                || queueDepthAfterPop >= ASYNC_WRITE_QUEUE_DEPTH_LOG_THRESHOLD;
            const std::string logMessage = StringUtil::Format(
                "Socket finished async write to {}: bytes={}, priority={}, transferred={}, queued_ms={}, write_ms={}, queue_depth={}, queue_bytes={}",
                Format(),
                messageSize,
                priority,
                bytes_transferred,
                queuedMs,
                writeMs,
                queueDepthAfterPop,
                queueBytesAfterPop);
            if (slowOrBacklogged) {
                LOG_DEBUG(logMessage);
            } else {
                LOG_TRACE(logMessage);
            }
        }
        StartAsyncWriteLocked();
    } else if (messageSize >= ASYNC_WRITE_LOG_THRESHOLD_BYTES) {
        const bool slowOrBacklogged = queuedMs >= ASYNC_WRITE_SLOW_LOG_THRESHOLD_MS
            || writeMs >= ASYNC_WRITE_SLOW_LOG_THRESHOLD_MS
            || queueDepthAfterPop >= ASYNC_WRITE_QUEUE_DEPTH_LOG_THRESHOLD;
        const std::string logMessage = StringUtil::Format(
            "Socket finished async write to {}: bytes={}, priority={}, transferred={}, queued_ms={}, write_ms={}, queue_depth={}, queue_bytes={}",
            Format(),
            messageSize,
            priority,
            bytes_transferred,
            queuedMs,
            writeMs,
            queueDepthAfterPop,
            queueBytesAfterPop);
        if (slowOrBacklogged) {
            LOG_DEBUG(logMessage);
        } else {
            LOG_TRACE(logMessage);
        }
    }
}

std::vector<uint8_t> Socket::ReceiveSync(const size_t num_bytes, const bool incrementCount)
{
    std::chrono::time_point timeout = std::chrono::system_clock::now() + std::chrono::seconds(10);
    while (!HasReceivedData()) {
        if (std::chrono::system_clock::now() >= timeout || !Global::IsRunning()) {
            LOG_TRACE(StringUtil::Format("ReceiveSync timed out waiting for {} bytes from {}", num_bytes, m_address));
            return {};
        }

        ThreadUtil::SleepFor(std::chrono::milliseconds(5));
    }

    std::vector<uint8_t> bytes(num_bytes);

    size_t numTries = 0;
    size_t bytesRead = 0;
    while (numTries++ < 5) {
        m_errorCode.clear();
        bytesRead += asio::read(*m_pSocket, asio::buffer(bytes.data() + bytesRead, num_bytes - bytesRead), m_errorCode);
        if (m_errorCode && m_errorCode.value() != EAGAIN && m_errorCode.value() != EWOULDBLOCK) {
            const size_t bytesToLog = (std::min)(bytesRead, static_cast<size_t>(32));
            bytes.resize(bytesRead);
            LOG_DEBUG(StringUtil::Format(
                "ReceiveSync failed reading {} bytes from {} after {} bytes. ec={} '{}', data={}",
                num_bytes,
                m_address,
                bytesRead,
                m_errorCode.value(),
                m_errorCode.message(),
                bytesToLog > 0 ? HexUtil::ConvertToHex(bytes, bytesToLog) : std::string("<empty>")
            ));
            ThrowSocketException(m_errorCode);
        }

        if (bytesRead == num_bytes) {
            if (incrementCount) {
                m_rateCounter.AddMessageReceived();
            }

            return bytes;
        } else if (m_errorCode.value() == EAGAIN || m_errorCode.value() == EWOULDBLOCK) {
            const size_t bytesToLog = (std::min)(bytesRead, static_cast<size_t>(32));
            LOG_DEBUG(StringUtil::Format(
                "ReceiveSync partial read from {}: {}/{} bytes after try {}. ec={} '{}', data={}",
                m_address,
                bytesRead,
                num_bytes,
                numTries,
                m_errorCode.value(),
                m_errorCode.message(),
                bytesToLog > 0 ? HexUtil::ConvertToHex(bytes, bytesToLog) : std::string("<empty>")
            ));
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }

    bytes.resize(bytesRead);
    const size_t bytesToLog = (std::min)(bytesRead, static_cast<size_t>(32));
    LOG_DEBUG(StringUtil::Format(
        "ReceiveSync exhausted retries reading {} bytes from {}. Returning {} bytes: {}",
        num_bytes,
        m_address,
        bytesRead,
        bytesToLog > 0 ? HexUtil::ConvertToHex(bytes, bytesToLog) : std::string("<empty>")
    ));
    return bytes;
}

bool Socket::HasReceivedData()
{
    const size_t available = m_pSocket->available(m_errorCode);
    if (m_errorCode && m_errorCode.value() != EAGAIN && m_errorCode.value() != EWOULDBLOCK) {
        ThrowSocketException(m_errorCode);
    }

    return available > 0;
}

void Socket::ThrowSocketException(const asio::error_code& ec)
{
    std::string error_message = "Socket error occurred";
    if (ec) {
        error_message = ec.message();
    }

#ifdef _WIN32
    const int lastError = WSAGetLastError();

    TCHAR* s = NULL;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&s, 0, NULL);

    error_message = StringUtil::ToUTF8(s);
    LocalFree(s);
#endif

    throw SocketException(ec, error_message);
}
