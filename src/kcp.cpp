#include <QtCore/qdatetime.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qbytearray.h>
#include <QtCore/qendian.h>
#include "../include/kcp.h"
#include "../include/socket_utils.h"
#include "kcp/ikcp.h"

QTNETWORKNG_NAMESPACE_BEGIN

const char PACKET_TYPE_UNCOMPRESSED_DATA = 0x01;
const char PACKET_TYPE_COMPRESSED_DATA = 0x02;
const char PACKET_TYPE_CLOSE= 0X03;
const char PACKET_TYPE_KEEPALIVE = 0x04;


class KcpSocketPrivate: public QObject
{
public:
    KcpSocketPrivate();
    virtual ~KcpSocketPrivate();
    static QSharedPointer<KcpSocket> create(KcpSocketPrivate *d) { return QSharedPointer<KcpSocket>(new KcpSocket(d)); }
public:
    virtual Socket::SocketError getError() const = 0;
    virtual QString getErrorString() const = 0;
    virtual bool isValid() const = 0;
    virtual QHostAddress localAddress() const = 0;
    virtual quint16 localPort() const = 0;
    QHostAddress peerAddress() const;
    QString peerName() const;
    quint16 peerPort() const;
    Socket::SocketType type() const;
    virtual Socket::NetworkLayerProtocol protocol() const = 0;
public:
    virtual QSharedPointer<KcpSocket> accept() = 0;
    virtual bool bind(QHostAddress &address, quint16 port, Socket::BindMode mode) = 0;
    virtual bool bind(quint16 port, Socket::BindMode mode) = 0;
    virtual bool connect(const QHostAddress &addr, quint16 port) = 0;
    virtual bool connect(const QString &hostName, quint16 port, Socket::NetworkLayerProtocol protocol) = 0;
    virtual bool close(bool force) = 0;
    virtual bool listen(int backlog) = 0;
    virtual bool setOption(Socket::SocketOption option, const QVariant &value) = 0;
    virtual QVariant option(Socket::SocketOption option) const = 0;
public:
    void setMode(KcpSocket::Mode mode);
    qint32 send(const char *data, qint32 size, bool all);
    qint32 recv(char *data, qint32 size, bool all);
    bool handleDatagram(const QByteArray &buf);
    void scheduleUpdate();
    void updateKcp();
    virtual qint32 rawSend(const char *data, qint32 size) = 0;

    QByteArray makeDataPacket(const char *data, qint32 size);
    QByteArray makeShutdownPacket();
    QByteArray makeKeepalivePacket();
public:
    QString errorString;
    Socket::SocketState state;
    Socket::SocketError error;

    QSharedPointer<Event> sendingQueueNotFull;
    QSharedPointer<Event> sendingQueueEmpty;
    QSharedPointer<Event> receivingQueueNotEmpty;
    QByteArray receivingBuffer;
    const quint32 waterLine;
    const quint64 zeroTimestamp;
    quint64 lastActiveTimestamp;
    quint64 lastKeepaliveTimestamp;
    quint64 tearDownTime;
    ikcpcb *kcp;
    int schedulerId;

    QHostAddress remoteAddress;
    quint16 remotePort;

    KcpSocket::Mode mode;
    bool compression;
};


static inline QString concat(const QHostAddress &addr, quint16 port)
{
    return addr.toString() + QString::number(port);
}


class MasterKcpSocketPrivate: public KcpSocketPrivate
{
public:
    MasterKcpSocketPrivate(Socket::NetworkLayerProtocol protocol);
    MasterKcpSocketPrivate(qintptr socketDescriptor);
    MasterKcpSocketPrivate(QSharedPointer<Socket> rawSocket);
    ~MasterKcpSocketPrivate();
public:
    virtual Socket::SocketError getError() const override;
    virtual QString getErrorString() const override;
    virtual bool isValid() const override;
    virtual QHostAddress localAddress() const override;
    virtual quint16 localPort() const override;
    virtual Socket::NetworkLayerProtocol protocol() const override;
public:
    virtual QSharedPointer<KcpSocket> accept() override;
    virtual bool bind(QHostAddress &address, quint16 port, Socket::BindMode mode) override;
    virtual bool bind(quint16 port, Socket::BindMode mode) override;
    virtual bool connect(const QHostAddress &addr, quint16 port) override;
    virtual bool connect(const QString &hostName, quint16 port, Socket::NetworkLayerProtocol protocol) override;
    virtual bool close(bool force) override;
    virtual bool listen(int backlog) override;
    virtual bool setOption(Socket::SocketOption option, const QVariant &value) override;
    virtual QVariant option(Socket::SocketOption option) const override;
public:
    virtual qint32 rawSend(const char *data, qint32 size) override;
public:
    void removeSlave(const QHostAddress &addr, quint16 port) { receivers.remove(concat(addr, port)); }
    void doReceive();
    void doAccept();
    bool startReceivingCoroutine();
public:
    QMap<QString, QPointer<class SlaveKcpSocketPrivate>> receivers;
    QSharedPointer<Socket> rawSocket;
    QSharedPointer<Coroutine> receivingCoroutine;
    Queue<QSharedPointer<KcpSocket>> pendingSlaves;
};


class SlaveKcpSocketPrivate: public KcpSocketPrivate
{
public:
    SlaveKcpSocketPrivate(MasterKcpSocketPrivate *parent, const QByteArray &buf, const QHostAddress &addr, quint16 port);
    ~SlaveKcpSocketPrivate();
public:
    virtual Socket::SocketError getError() const override;
    virtual QString getErrorString() const override;
    virtual bool isValid() const override;
    virtual QHostAddress localAddress() const override;
    virtual quint16 localPort() const override;
    virtual Socket::NetworkLayerProtocol protocol() const override;
public:
    virtual QSharedPointer<KcpSocket> accept() override;
    virtual bool bind(QHostAddress &address, quint16 port, Socket::BindMode mode) override;
    virtual bool bind(quint16 port, Socket::BindMode mode) override;
    virtual bool connect(const QHostAddress &addr, quint16 port) override;
    virtual bool connect(const QString &hostName, quint16 port, Socket::NetworkLayerProtocol protocol) override;
    virtual bool close(bool force) override;
    virtual bool listen(int backlog) override;
    virtual bool setOption(Socket::SocketOption option, const QVariant &value) override;
    virtual QVariant option(Socket::SocketOption option) const override;
public:
    virtual qint32 rawSend(const char *data, qint32 size) override;
public:
    QPointer<MasterKcpSocketPrivate> parent;
};


int kcp_callback(const char *buf, int len, ikcpcb *, void *user)
{
    KcpSocketPrivate *p = static_cast<KcpSocketPrivate*>(user);
    const QByteArray &packet = p->makeDataPacket(buf, len);
    if (p->rawSend(packet.data(), packet.size()) != packet.size()) {
        p->close(true);
    }
}

KcpSocketPrivate::KcpSocketPrivate()
    :state(Socket::UnconnectedState), error(Socket::NoError)
    , sendingQueueNotFull(new Event()), sendingQueueEmpty(new Event()), receivingQueueNotEmpty(new Event())
    , waterLine(32), zeroTimestamp(QDateTime::currentMSecsSinceEpoch()), lastActiveTimestamp(zeroTimestamp)
    , lastKeepaliveTimestamp(zeroTimestamp),tearDownTime(1000 * 30), schedulerId(0), remotePort(0)
    , mode(KcpSocket::Internet), compression(false)
{
    kcp = ikcp_create(0, this);
    ikcp_setoutput(kcp, kcp_callback);
    sendingQueueEmpty->set();
    sendingQueueNotFull->set();
    receivingQueueNotEmpty->clear();
    setMode(mode);
}


KcpSocketPrivate::~KcpSocketPrivate()
{
    ikcp_release(kcp);
}


QHostAddress KcpSocketPrivate::peerAddress() const
{
    return remoteAddress;
}


QString KcpSocketPrivate::peerName() const
{
    return remoteAddress.toString();
}


quint16 KcpSocketPrivate::peerPort() const
{
    return remotePort;
}


Socket::SocketType KcpSocketPrivate::type() const
{
    return Socket::KcpSocket;
}


//bool KcpSocketPrivate::close()
//{
//    if (state == Socket::ConnectedState) {
//        ikcp_flush(kcp);
//        bool ok = sendingQueueEmpty->wait();
//        Q_UNUSED(ok);
//        state = Socket::UnconnectedState;
//    }
//    if (schedulerId) {
//        EventLoopCoroutine::get()->cancelCall(schedulerId);
//        schedulerId = 0;
//    }
//    return true;
//}

void KcpSocketPrivate::setMode(KcpSocket::Mode mode)
{
    this->mode = mode;
    switch (mode) {
    case KcpSocket::Internet:
        ikcp_nodelay(kcp, 0, 10, 0, 0);
        ikcp_setmtu(kcp, 1400);
        ikcp_wndsize(kcp, 1024, 1024);
        break;
    case KcpSocket::Ethernet:
        ikcp_nodelay(kcp, 1, 10, 1, 1);
        ikcp_setmtu(kcp, 16384);
        ikcp_wndsize(kcp, 64, 64);
        break;
    case KcpSocket::Loopback:
        ikcp_nodelay(kcp, 1, 10, 2, 1);
        ikcp_setmtu(kcp, 32768);
        ikcp_wndsize(kcp, 32, 32);
        break;
    }
}

qint32 KcpSocketPrivate::send(const char *data, qint32 size, bool all)
{
    int count = 0;
    while (count < size) {
        if (state != Socket::ConnectedState) {
            return -1;
        }
        qint32 nextBlockSize = qMin(1024 * 8, size - count);
        int result = ikcp_send(kcp, data + count, nextBlockSize);
        if (result < 0) {
            if (count > 0 && !all) {
                return count;
            } else {
                updateKcp();
                bool ok = sendingQueueEmpty->wait();
                if (!ok) {
                    return -1;
                }
            }
        } else {
            count += nextBlockSize;
            updateKcp();
        }
    }
//    while (ikcp_waitsnd(kcp) > 0 && isValid()) {
//        ikcp_flush(kcp);
//    }
    int sendingQueueSize = ikcp_waitsnd(kcp);
    if (sendingQueueSize > waterLine * 1.2) {
        sendingQueueNotFull->clear();
    }
    bool ok = sendingQueueNotFull->wait();
    if (!ok) {
        return -1;
    }

    return isValid() ? count : -1;
}


qint32 KcpSocketPrivate::recv(char *data, qint32 size, bool all)
{
    while (true) {
        int peeksize = ikcp_peeksize(kcp);
        if (peeksize > 0) {
            QByteArray buf(peeksize, Qt::Uninitialized);
            int readBytes = ikcp_recv(kcp, buf.data(), buf.size());
            Q_ASSERT(readBytes == peeksize);
            if (receivingBuffer.isEmpty()) {
                receivingBuffer = buf;
            } else {
                receivingBuffer.append(buf);
            }
        }
        if (!receivingBuffer.isEmpty()) {
            if (!all || receivingBuffer.size() >= size) {
                qint32 len = qMin(size, receivingBuffer.size());
                memcpy(data, receivingBuffer.data(), static_cast<size_t>(len));
                receivingBuffer.remove(0, len);
                return len;
            }
        }
        receivingQueueNotEmpty->clear();
        bool ok = receivingQueueNotEmpty->wait();
        if (!ok) {
            return -1;
        }
    }
}

bool KcpSocketPrivate::handleDatagram(const QByteArray &buf)
{
    if (buf.isEmpty()) {
        return true;
    }

    int dataSize;
    switch(buf.at(0)) {
    case PACKET_TYPE_COMPRESSED_DATA:
    case PACKET_TYPE_UNCOMPRESSED_DATA:
        if (buf.size() < 3) {
            qDebug() << "invalid packet. buf.size() < 3";
            return true;
        }
#if (QT_VERSION >= QT_VERSION_CHECK(5, 7, 0))
        dataSize = qFromBigEndian<quint16>(reinterpret_cast<const void*>(buf.constData() + 1));
#else
        dataSize = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(buf.constData() + 1));
#endif
        if (dataSize != buf.size() - 3) {
            qDebug() << "invalid packet. dataSize != buf.size() - 3";
            return true;
        }

        int result;
        if (buf.at(0) == PACKET_TYPE_UNCOMPRESSED_DATA) {
            result = ikcp_input(kcp, buf.data() + 3, dataSize);
        } else {
            const QByteArray &uncompressed = qUncompress(reinterpret_cast<const uchar*>(buf.data() + 3), dataSize);
            result = ikcp_input(kcp, uncompressed.constData(), uncompressed.size());
        }
        if (result < 0) {
            // invalid datagram
            qDebug() << "invalid datagram. kcp returns" << result;
        } else {
            lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();
            receivingQueueNotEmpty->set();
            updateKcp();
        }
        break;
    case PACKET_TYPE_CLOSE:
        close(true);
        break;
    case PACKET_TYPE_KEEPALIVE:
        lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();
        break;
    default:
        break;
    }
    return true;
}


struct UpdateKcpFunctor: public Functor
{
public:
    UpdateKcpFunctor(KcpSocketPrivate *p)
        :p(p) {}
    virtual void operator()() override;
private:
    QPointer<KcpSocketPrivate> p;
};


void UpdateKcpFunctor::operator ()()
{
    if (!p.isNull()) {
        p->updateKcp();
    }
}


void KcpSocketPrivate::scheduleUpdate()
{
    if (schedulerId) {
        return;
    }
    const quint64 &now = QDateTime::currentMSecsSinceEpoch();
    quint32 current = static_cast<quint32>(now - zeroTimestamp);  // impossible to overflow.
    quint32 ts = ikcp_check(kcp, current);
    quint32 interval = ts - current;
    schedulerId = EventLoopCoroutine::get()->callLater(interval, new UpdateKcpFunctor(this));
}


void KcpSocketPrivate::updateKcp()
{
    const quint64 &now = QDateTime::currentMSecsSinceEpoch();

    if (now - lastActiveTimestamp > tearDownTime) {
        qDebug() << "tear down.";
        close(true);
        return;
    }

    quint32 current = static_cast<quint32>(now - zeroTimestamp);  // impossible to overflow.
    ikcp_update(kcp, current);

    if (now - lastKeepaliveTimestamp > 1000 * 5) {
        const QByteArray &packet = makeKeepalivePacket();
        if (rawSend(packet.data(), packet.size()) != packet.size()) {
            close(true);
            return;
        }
    }

    int sendingQueueSize = ikcp_waitsnd(kcp);
    if (sendingQueueSize <= 0) {
        sendingQueueEmpty->set();
        sendingQueueNotFull->set();
    } else {
        sendingQueueEmpty->clear();
        if (sendingQueueSize > waterLine) {
            sendingQueueNotFull->clear();
        } else {
            sendingQueueNotFull->set();
        }
    }

    if (Q_LIKELY(schedulerId)) {
        EventLoopCoroutine::get()->cancelCall(schedulerId);
        schedulerId = 0;
    }
    scheduleUpdate();
}


QByteArray KcpSocketPrivate::makeDataPacket(const char *data, qint32 size)
{
    QByteArray packet;

    if (compression) {
        QByteArray compressed = qCompress(reinterpret_cast<const uchar*>(data), size);
        if (compressed.size() < size) {
            packet.reserve(3 + compressed.size());
            packet.append(PACKET_TYPE_COMPRESSED_DATA);
            packet.append(static_cast<char>((compressed.size() >> 8) & 0xff));
            packet.append(static_cast<char>(compressed.size() & 0xff));
            packet.append(compressed);
            return packet;
        }
    }

    packet.reserve(3 + size);
    packet.append(PACKET_TYPE_UNCOMPRESSED_DATA);
    packet.append(static_cast<char>((size >> 8) & 0xff));
    packet.append(static_cast<char>(size & 0xff));
    packet.append(data, size);
    return packet;
}


QByteArray KcpSocketPrivate::makeShutdownPacket()
{
    QByteArray packet;
    packet.append(PACKET_TYPE_CLOSE);
    return packet;
}


QByteArray KcpSocketPrivate::makeKeepalivePacket()
{
    QByteArray packet;
    packet.append(PACKET_TYPE_KEEPALIVE);
    return packet;
}


MasterKcpSocketPrivate::MasterKcpSocketPrivate(Socket::NetworkLayerProtocol protocol)
    :rawSocket(new Socket(protocol, Socket::UdpSocket))
{
//    rawSocket->setOption(Socket::SendBufferSizeSocketOption, 1024 * 1024 * 8);
}


MasterKcpSocketPrivate::MasterKcpSocketPrivate(qintptr socketDescriptor)
    :rawSocket(new Socket(socketDescriptor))
{
//    rawSocket->setOption(Socket::SendBufferSizeSocketOption, 1024 * 1024 * 8);
}


MasterKcpSocketPrivate::MasterKcpSocketPrivate(QSharedPointer<Socket> rawSocket)
    :rawSocket(rawSocket)
{
//    rawSocket->setOption(Socket::SendBufferSizeSocketOption, 1024 * 1024 * 8);
}


MasterKcpSocketPrivate::~MasterKcpSocketPrivate()
{
    close(false);
}

Socket::SocketError MasterKcpSocketPrivate::getError() const
{
    if (error != Socket::NoError) {
        return error;
    } else {
        return rawSocket->error();
    }
}


QString MasterKcpSocketPrivate::getErrorString() const
{
    if (!errorString.isEmpty()) {
        return errorString;
    } else {
        return rawSocket->errorString();
    }
}


bool MasterKcpSocketPrivate::isValid() const
{
    return state == Socket::ConnectedState || state == Socket::BoundState || state == Socket::ListeningState;
}


QHostAddress MasterKcpSocketPrivate::localAddress() const
{
    return rawSocket->localAddress();
}


quint16 MasterKcpSocketPrivate::localPort() const
{
    return rawSocket->localPort();
}


Socket::NetworkLayerProtocol MasterKcpSocketPrivate::protocol() const
{
    return rawSocket->protocol();
}


bool MasterKcpSocketPrivate::close(bool force)
{
    if (state == Socket::UnconnectedState) {
        return true;
    } else if (state == Socket::ConnectedState) {
        state = Socket::UnconnectedState;
        if (!force) {
            updateKcp();
            bool ok = sendingQueueEmpty->wait();
            const QByteArray &packet = makeShutdownPacket();
            rawSend(packet.constData(), packet.size());
            Q_UNUSED(ok);
        }
    } else if (state == Socket::ListeningState) {
        state = Socket::UnconnectedState;
        for (QPointer<SlaveKcpSocketPrivate> receiver: receivers.values()) {
            if (!receiver.isNull()) {
                receiver->close(force);
            }
        }
        receivers.clear();
    } else {  // BoundState
        state = Socket::UnconnectedState;
        rawSocket->close();
        return true;
    }

    rawSocket->close();

    //connected and listen state would do more cleaning work.
    if (schedulerId) {
        EventLoopCoroutine::get()->cancelCall(schedulerId);
        schedulerId = 0;
    }
    if (!receivingCoroutine.isNull()) {
        receivingCoroutine->kill();
        receivingCoroutine->join();
    }
    // await all pending recv()/send()
    receivingQueueNotEmpty->set();
    sendingQueueEmpty->set();
    sendingQueueNotFull->set();
    return true;
}


bool MasterKcpSocketPrivate::listen(int backlog)
{
    if (state != Socket::BoundState) {
        return false;
    }
    state = Socket::ListeningState;
    pendingSlaves.setCapacity(backlog);
    return true;
}


void MasterKcpSocketPrivate::doReceive()
{
    QHostAddress addr;
    quint16 port;
    QByteArray buf;
    while (true) {
        buf.resize(1024 * 64);
        qint32 bytes = rawSocket->recvfrom(buf.data(), buf.size(), &addr, &port);
        if (Q_UNLIKELY(bytes < 0 || addr.isNull() || port == 0)) {
            close(true);
            return;
        }
//        if (Q_UNLIKELY(addr.toIPv6Address() != remoteAddress.toIPv6Address() || port != remotePort)) {
//            // not my packet.
//            qDebug() << "not my packet:" << addr << remoteAddress << port;
//            continue;
//        }
        buf.resize(bytes);
        if (!handleDatagram(buf)) {
            close(true);
            return;
        }
    }
}


void MasterKcpSocketPrivate::doAccept()
{
    QHostAddress addr;
    quint16 port;
    QByteArray buf;
    while (true) {
        buf.resize(1024 * 64);
        qint32 bytes = rawSocket->recvfrom(buf.data(), buf.size(), &addr, &port);
        if (bytes < 0 || addr.isNull() || port == 0) {
            close(true);
            return;
        }
        buf.resize(bytes);
        const QString &key = concat(addr, port);
        if (receivers.contains(key)) {
            QPointer<SlaveKcpSocketPrivate> receiver = receivers.value(key);
            if (!receiver.isNull()) {
                QElapsedTimer t;
                t.start();
                if (!receiver->handleDatagram(buf)) {
                    receivers.remove(key);
                }
            }
        } else {
            if (pendingSlaves.size() < pendingSlaves.capacity()) {  // not full.
                SlaveKcpSocketPrivate *d = new SlaveKcpSocketPrivate(this, buf, addr, port);
                d->setMode(this->mode);
                receivers.insert(key, d);
                pendingSlaves.put(KcpSocketPrivate::create(d));
            }
        }
    }
}

bool MasterKcpSocketPrivate::startReceivingCoroutine()
{
    if (!receivingCoroutine.isNull()) {
        return true;
    }
    switch (state) {
    case Socket::UnconnectedState:
    case Socket::BoundState:
    case Socket::ConnectingState:
    case Socket::HostLookupState:
    case Socket::ClosingState:
        return false;
    case Socket::ConnectedState:
        receivingCoroutine.reset(Coroutine::spawn([this] { doReceive(); }));
        break;
    case Socket::ListeningState:
        receivingCoroutine.reset(Coroutine::spawn([this] { doAccept(); }));
        break;
    }
    return true;
}

QSharedPointer<KcpSocket> MasterKcpSocketPrivate::accept()
{
    if (state != Socket::ListeningState) {
        return QSharedPointer<KcpSocket>();
    }
    startReceivingCoroutine();
    return pendingSlaves.get();
}


bool MasterKcpSocketPrivate::connect(const QHostAddress &addr, quint16 port)
{
    if (state != Socket::UnconnectedState && state != Socket::BoundState) {
        return false;
    }
    remoteAddress = addr;
    remotePort = port;
    state = Socket::ConnectedState;
    return true;
}


bool MasterKcpSocketPrivate::connect(const QString &hostName, quint16 port, Socket::NetworkLayerProtocol protocol)
{
    if (state != Socket::UnconnectedState && state != Socket::BoundState) {
        return false;
    }
    if (rawSocket->connect(hostName, port, protocol))  {
        remoteAddress = rawSocket->peerAddress();
        remotePort = port;
        state = Socket::ConnectedState;
        return true;
    } else {
        return false;
    }
}


qint32 MasterKcpSocketPrivate::rawSend(const char *data, qint32 size)
{
    // can be called from close() under UnconnectedState
//    if (state != Socket::ConnectedState) {
//        return -1;
//    }
    lastKeepaliveTimestamp = QDateTime::currentMSecsSinceEpoch();
    startReceivingCoroutine();

    int count = 0;
    while (count < size) {
        qint32 sentBytes = rawSocket->sendto(data + count, size - count,
                                             remoteAddress, remotePort);
        if (sentBytes < 0) {
            return count;
        } else {
            count += sentBytes;
        }
    }
    return count;
}


bool MasterKcpSocketPrivate::bind(QHostAddress &address, quint16 port, Socket::BindMode mode)
{
    if (state != Socket::UnconnectedState) {
        return false;
    }
    if(mode & Socket::ReuseAddressHint) {
        rawSocket->setOption(Socket::AddressReusable, true);
    }
    if (rawSocket->bind(address, port, mode)) {
        state = Socket::BoundState;
        return true;
    } else {
        return false;
    }
}


bool MasterKcpSocketPrivate::bind(quint16 port, Socket::BindMode mode)
{
    if (state != Socket::UnconnectedState) {
        return false;
    }
    if(mode & Socket::ReuseAddressHint) {
        rawSocket->setOption(Socket::AddressReusable, true);
    }
    if (rawSocket->bind(port, mode)) {
        state = Socket::BoundState;
        return true;
    } else {
        return false;
    }
}


bool MasterKcpSocketPrivate::setOption(Socket::SocketOption option, const QVariant &value)
{
    return rawSocket->setOption(option, value);
}


QVariant MasterKcpSocketPrivate::option(Socket::SocketOption option) const
{
    return rawSocket->option(option);
}


SlaveKcpSocketPrivate::SlaveKcpSocketPrivate(MasterKcpSocketPrivate *parent, const QByteArray &buf, const QHostAddress &addr, quint16 port)
    :parent(parent)
{
    remoteAddress = addr;
    remotePort = port;
    state = Socket::ConnectedState;
    handleDatagram(buf);
}


SlaveKcpSocketPrivate::~SlaveKcpSocketPrivate()
{
    close(false);
}


Socket::SocketError SlaveKcpSocketPrivate::getError() const
{
    if (error != Socket::NoError) {
        return error;
    } else {
        if (!parent.isNull()) {
            return parent->rawSocket->error();
        } else {
            return Socket::SocketAccessError;
        }
    }
}


QString SlaveKcpSocketPrivate::getErrorString() const
{
    if (!errorString.isEmpty()) {
        return errorString;
    } else {
        if (!parent.isNull()) {
            return parent->rawSocket->errorString();
        } else {
            return QString::fromLatin1("Invalid socket descriptor");
        }
    }
}


bool SlaveKcpSocketPrivate::isValid() const
{
    return state == Socket::ConnectedState && !parent.isNull();
}


QHostAddress SlaveKcpSocketPrivate::localAddress() const
{
    if (parent.isNull()) {
        return QHostAddress();
    }
    return parent->rawSocket->localAddress();
}


quint16 SlaveKcpSocketPrivate::localPort() const
{
    if (parent.isNull()) {
        return -1;
    }
    return parent->rawSocket->localPort();
}


Socket::NetworkLayerProtocol SlaveKcpSocketPrivate::protocol() const
{
    if (parent.isNull()) {
        return Socket::UnknownNetworkLayerProtocol;
    }
    return parent->rawSocket->protocol();
}


bool SlaveKcpSocketPrivate::close(bool force)
{
    if (state == Socket::UnconnectedState) {
        return true;
    } else if (state == Socket::ConnectedState) {
        state = Socket::UnconnectedState;
        if (!force) {
            updateKcp();
            bool ok = sendingQueueEmpty->wait();
            Q_UNUSED(ok);
            const QByteArray &packet = makeShutdownPacket();
            rawSend(packet.constData(), packet.size());
        }
    } else {  // there can be no other states.
        state = Socket::UnconnectedState;
    }
    if (schedulerId) {
        EventLoopCoroutine::get()->cancelCall(schedulerId);
        schedulerId = 0;
    }
    if (!parent.isNull()) {
        parent->removeSlave(remoteAddress, remotePort);
        parent.clear();
    }
    // await all pending recv()/send()
    receivingQueueNotEmpty->set();
    sendingQueueEmpty->set();
    sendingQueueNotFull->set();
    return true;
}


bool SlaveKcpSocketPrivate::listen(int backlog)
{
    return false;
}


QSharedPointer<KcpSocket> SlaveKcpSocketPrivate::accept()
{
    return QSharedPointer<KcpSocket>();
}


bool SlaveKcpSocketPrivate::connect(const QHostAddress &addr, quint16 port)
{
    return false;
}


bool SlaveKcpSocketPrivate::connect(const QString &hostName, quint16 port, Socket::NetworkLayerProtocol protocol)
{
    return false;
}

qint32 SlaveKcpSocketPrivate::rawSend(const char *data, qint32 size)
{
    if (parent.isNull()) {
        return -1;
    } else {
        lastKeepaliveTimestamp = QDateTime::currentMSecsSinceEpoch();
        int count = 0;
        while (count < size) {
            qint32 sentBytes = parent->rawSocket->sendto(data + count, size - count, remoteAddress, remotePort);
            if (sentBytes < 0) {
                return count;
            } else {
                count += sentBytes;
            }
        }
        return count;
    }
}

bool SlaveKcpSocketPrivate::bind(QHostAddress &, quint16, Socket::BindMode)
{
    return false;
}


bool SlaveKcpSocketPrivate::bind(quint16, Socket::BindMode)
{
    return false;
}


bool SlaveKcpSocketPrivate::setOption(Socket::SocketOption, const QVariant &)
{
    return false;
}


QVariant SlaveKcpSocketPrivate::option(Socket::SocketOption option) const
{
    if (parent.isNull()) {
        return QVariant();
    } else {
        return parent->rawSocket->option(option);
    }
}


KcpSocket::KcpSocket(Socket::NetworkLayerProtocol protocol)
    :d_ptr(new MasterKcpSocketPrivate(protocol))
{
}


KcpSocket::KcpSocket(qintptr socketDescriptor)
    :d_ptr(new MasterKcpSocketPrivate(socketDescriptor))
{

}


KcpSocket::KcpSocket(QSharedPointer<Socket> rawSocket)
    :d_ptr(new MasterKcpSocketPrivate(rawSocket))
{

}


KcpSocket::KcpSocket(KcpSocketPrivate *d)
    :d_ptr(d)
{

}


KcpSocket::~KcpSocket()
{
    delete d_ptr;
}


void KcpSocket::setMode(Mode mode)
{
    Q_D(KcpSocket);
    d->setMode(mode);
}


KcpSocket::Mode KcpSocket::mode() const
{
    Q_D(const KcpSocket);
    return d->mode;
}


void KcpSocket::setCompression(bool compression)
{
    Q_D(KcpSocket);
    d->compression = compression;
}


bool KcpSocket::compression() const
{
    Q_D(const KcpSocket);
    return d->compression;
}


Socket::SocketError KcpSocket::error() const
{
    Q_D(const KcpSocket);
    return d->getError();
}


QString KcpSocket::errorString() const
{
    Q_D(const KcpSocket);
    return d->getErrorString();
}


bool KcpSocket::isValid() const
{
    Q_D(const KcpSocket);
    return d->isValid();
}


QHostAddress KcpSocket::localAddress() const
{
    Q_D(const KcpSocket);
    return d->localAddress();
}


quint16 KcpSocket::localPort() const
{
    Q_D(const KcpSocket);
    return d->localPort();
}


QHostAddress KcpSocket::peerAddress() const
{
    Q_D(const KcpSocket);
    return d->peerAddress();
}


QString KcpSocket::peerName() const
{
    Q_D(const KcpSocket);
    return d->peerName();
}


quint16 KcpSocket::peerPort() const
{
    Q_D(const KcpSocket);
    return d->peerPort();
}


Socket::SocketType KcpSocket::type() const
{
    Q_D(const KcpSocket);
    return d->type();
}


Socket::SocketState KcpSocket::state() const
{
    Q_D(const KcpSocket);
    return d->state;
}


Socket::NetworkLayerProtocol KcpSocket::protocol() const
{
    Q_D(const KcpSocket);
    return d->protocol();
}


QSharedPointer<KcpSocket> KcpSocket::accept()
{
    Q_D(KcpSocket);
    return d->accept();
}


bool KcpSocket::bind(QHostAddress &address, quint16 port, Socket::BindMode mode)
{
    Q_D(KcpSocket);
    return d->bind(address, port, mode);
}


bool KcpSocket::bind(quint16 port, Socket::BindMode mode)
{
    Q_D(KcpSocket);
    return d->bind(port, mode);
}


bool KcpSocket::connect(const QHostAddress &addr, quint16 port)
{
    Q_D(KcpSocket);
    return d->connect(addr, port);
}


bool KcpSocket::connect(const QString &hostName, quint16 port, Socket::NetworkLayerProtocol protocol)
{
    Q_D(KcpSocket);
    return d->connect(hostName, port, protocol);
}


bool KcpSocket::close()
{
    Q_D(KcpSocket);
    return d->close(false);
}


bool KcpSocket::listen(int backlog)
{
    Q_D(KcpSocket);
    return d->listen(backlog);
}


bool KcpSocket::setOption(Socket::SocketOption option, const QVariant &value)
{
    Q_D(KcpSocket);
    return d->setOption(option, value);
}


QVariant KcpSocket::option(Socket::SocketOption option) const
{
    Q_D(const KcpSocket);
    return d->option(option);
}


qint32 KcpSocket::recv(char *data, qint32 size)
{
    Q_D(KcpSocket);
    return d->recv(data, size, false);
}


qint32 KcpSocket::recvall(char *data, qint32 size)
{
    Q_D(KcpSocket);
    return d->recv(data, size, true);
}


qint32 KcpSocket::send(const char *data, qint32 size)
{
    Q_D(KcpSocket);
    qint32 bytesSent = d->send(data, size, false);
    if(bytesSent == 0 && !d->isValid()) {
        return -1;
    } else {
        return bytesSent;
    }
}


qint32 KcpSocket::sendall(const char *data, qint32 size)
{
    Q_D(KcpSocket);
    return d->send(data, size, true);
}


QByteArray KcpSocket::recv(qint32 size)
{
    Q_D(KcpSocket);
    QByteArray bs;
    bs.resize(size);

    qint32 bytes = d->recv(bs.data(), bs.size(), false);
    if(bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return QByteArray();
}


QByteArray KcpSocket::recvall(qint32 size)
{
    Q_D(KcpSocket);
    QByteArray bs;
    bs.resize(size);

    qint32 bytes = d->recv(bs.data(), bs.size(), true);
    if(bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return QByteArray();
}


qint32 KcpSocket::send(const QByteArray &data)
{
    Q_D(KcpSocket);
    qint32 bytesSent = d->send(data.data(), data.size(), false);
    if(bytesSent == 0 && !d->isValid()) {
        return -1;
    } else {
        return bytesSent;
    }
}


qint32 KcpSocket::sendall(const QByteArray &data)
{
    Q_D(KcpSocket);
    return d->send(data.data(), data.size(), true);
}


namespace {

class SocketLikeImpl: public SocketLike
{
public:
    SocketLikeImpl(QSharedPointer<KcpSocket> s);
public:
    virtual Socket::SocketError error() const override;
    virtual QString errorString() const override;
    virtual bool isValid() const override;
    virtual QHostAddress localAddress() const override;
    virtual quint16 localPort() const override;
    virtual QHostAddress peerAddress() const override;
    virtual QString peerName() const override;
    virtual quint16 peerPort() const override;
    virtual qintptr	fileno() const override;
    virtual Socket::SocketType type() const override;
    virtual Socket::SocketState state() const override;
    virtual Socket::NetworkLayerProtocol protocol() const override;

    virtual Socket *acceptRaw() override;
    virtual QSharedPointer<SocketLike> accept() override;
    virtual bool bind(QHostAddress &address, quint16 port, Socket::BindMode mode) override;
    virtual bool bind(quint16 port, Socket::BindMode mode) override;
    virtual bool connect(const QHostAddress &addr, quint16 port) override;
    virtual bool connect(const QString &hostName, quint16 port, Socket::NetworkLayerProtocol protocol) override;
    virtual bool close() override;
    virtual bool listen(int backlog) override;
    virtual bool setOption(Socket::SocketOption option, const QVariant &value) override;
    virtual QVariant option(Socket::SocketOption option) const override;

    virtual qint32 recv(char *data, qint32 size) override;
    virtual qint32 recvall(char *data, qint32 size) override;
    virtual qint32 send(const char *data, qint32 size) override;
    virtual qint32 sendall(const char *data, qint32 size) override;
    virtual QByteArray recv(qint32 size) override;
    virtual QByteArray recvall(qint32 size) override;
    virtual qint32 send(const QByteArray &data) override;
    virtual qint32 sendall(const QByteArray &data) override;
public:
    QSharedPointer<KcpSocket> s;
};

SocketLikeImpl::SocketLikeImpl(QSharedPointer<KcpSocket> s)
    :s(s) {}

Socket::SocketError SocketLikeImpl::error() const
{
    return s->error();
}

QString SocketLikeImpl::errorString() const
{
    return s->errorString();
}

bool SocketLikeImpl::isValid() const
{
    return s->isValid();
}

QHostAddress SocketLikeImpl::localAddress() const
{
    return s->localAddress();
}

quint16 SocketLikeImpl::localPort() const
{
    return s->localPort();
}

QHostAddress SocketLikeImpl::peerAddress() const
{
    return s->peerAddress();
}

QString SocketLikeImpl::peerName() const
{
    return s->peerName();
}

quint16 SocketLikeImpl::peerPort() const
{
    return s->peerPort();
}

qintptr	SocketLikeImpl::fileno() const
{
    return -1;
}

Socket::SocketType SocketLikeImpl::type() const
{
    return s->type();
}

Socket::SocketState SocketLikeImpl::state() const
{
    return s->state();
}

Socket::NetworkLayerProtocol SocketLikeImpl::protocol() const
{
    return s->protocol();
}

Socket *SocketLikeImpl::acceptRaw()
{
    return nullptr;
}

QSharedPointer<SocketLike> SocketLikeImpl::accept()
{
    return SocketLike::kcpSocket(s->accept());
}

bool SocketLikeImpl::bind(QHostAddress &address, quint16 port = 0, Socket::BindMode mode = Socket::DefaultForPlatform)
{
    return s->bind(address, port, mode);
}

bool SocketLikeImpl::bind(quint16 port, Socket::BindMode mode)
{
    return s->bind(port, mode);
}

bool SocketLikeImpl::connect(const QHostAddress &addr, quint16 port)
{
    return s->connect(addr, port);
}

bool SocketLikeImpl::connect(const QString &hostName, quint16 port, Socket::NetworkLayerProtocol protocol)
{
    return s->connect(hostName, port, protocol);
}

bool SocketLikeImpl::close()
{
    return s->close();
}

bool SocketLikeImpl::listen(int backlog)
{
    return s->listen(backlog);
}

bool SocketLikeImpl::setOption(Socket::SocketOption option, const QVariant &value)
{
    return s->setOption(option, value);
}

QVariant SocketLikeImpl::option(Socket::SocketOption option) const
{
    return s->option(option);
}

qint32 SocketLikeImpl::recv(char *data, qint32 size)
{
    return s->recv(data, size);
}

qint32 SocketLikeImpl::recvall(char *data, qint32 size)
{
    return s->recvall(data, size);
}

qint32 SocketLikeImpl::send(const char *data, qint32 size)
{
    return s->send(data, size);
}

qint32 SocketLikeImpl::sendall(const char *data, qint32 size)
{
    return s->sendall(data, size);
}

QByteArray SocketLikeImpl::recv(qint32 size)
{
    return s->recv(size);
}

QByteArray SocketLikeImpl::recvall(qint32 size)
{
    return s->recvall(size);
}

qint32 SocketLikeImpl::send(const QByteArray &data)
{
    return s->send(data);
}

qint32 SocketLikeImpl::sendall(const QByteArray &data)
{
    return s->sendall(data);
}

}

QSharedPointer<SocketLike> SocketLike::kcpSocket(QSharedPointer<KcpSocket> s)
{
    return QSharedPointer<SocketLikeImpl>::create(s).dynamicCast<SocketLike>();
}

QSharedPointer<SocketLike> SocketLike::kcpSocket(KcpSocket *s)
{
    return QSharedPointer<SocketLikeImpl>::create(QSharedPointer<KcpSocket>(s)).dynamicCast<SocketLike>();
}

QSharedPointer<KcpSocket> convertSocketLikeToKcpSocket(QSharedPointer<SocketLike> socket)
{
    QSharedPointer<SocketLikeImpl> impl = socket.dynamicCast<SocketLikeImpl>();
    if (impl.isNull()) {
        return QSharedPointer<KcpSocket>();
    } else {
        return impl->s;
    }
}

QTNETWORKNG_NAMESPACE_END
