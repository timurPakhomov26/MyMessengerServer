#include "server.h"
#include <QStringList>
#include <QDateTime>

Server::Server(QObject *parent) : QObject(parent)
{
    m_server = new QTcpServer(this);
    m_startTime = QDateTime::currentDateTime();

    connect(m_server, &QTcpServer::newConnection, this, &Server::onNewConnection);

    if (m_server->listen(QHostAddress::Any, 1234))
        log("Server started on port 1234");

    else
        log("Server failed to start!", LogLevel::Error);
}

// Функция рассылки списка имен всем подключенным
void Server::broadcastUserList()
{
    QStringList users = m_clients.keys();
    sendToAll("USERS_LIST:" + users.join(","));
}

void Server::onNewConnection()
{
    QTcpSocket *socket = m_server->nextPendingConnection();

    connect(socket, &QTcpSocket::readyRead, this, &Server::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &Server::onDisconnected);
    log("New attempt of connection...");
}

void Server::onReadyRead()
{
    auto *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    QString data = QString::fromUtf8(socket->readAll()).trimmed();
    if (data.isEmpty()) return;

    if (data == "/uptime")
    {
        socket->write(QString("SERVER: My uptime is %1").arg(getUptime()).toUtf8());
        return;
    }

    if (data == "/help")
    {
        socket->write("SERVER: Доступны команды: /uptime, /me, /help");
        return;
    }

    if (data.startsWith("/me "))
    {
        QString senderName = m_clients.key(socket);
        QString action = data.mid(4);
        sendToAll("* " + senderName + " " + action);
        return;
    }

    if (!m_clients.values().contains(socket))
    {
        if (isValidName(data))
        {
            m_clients[data] = socket;
            sendToAll("SYSTEM: Пользователь [" + data + "] вошел в чат");
            broadcastUserList();
            log("User registered: " + data);
        }
        else
        {
            log("Rejected nick: " + data, LogLevel::Warning);
            socket->write("SYSTEM: Invalid nickname!");
            socket->disconnectFromHost();
        }
        return;
    }

    if (data.contains(":"))
    {
        QString target = data.section(':', 0, 0);
        QString text = data.section(':', 1);
        QString senderName = m_clients.key(socket);

        if (m_clients.contains(target))
        {
            m_clients[target]->write(QString("%1: %2").arg(senderName, text).toUtf8());
        }
        else
        {
            socket->write("SYSTEM: User not found.");
        }
    }
}

void Server::onDisconnected()
{
    auto *socket = qobject_cast<QTcpSocket*>(sender());
    QString name = m_clients.key(socket);
    if (!name.isEmpty())
    {
        m_clients.remove(name);
        sendToAll("SYSTEM: Пользователь [" + name + "] покинул чат");
        broadcastUserList();
        log("User disconnected: " + name);
    }
    socket->deleteLater();
}

QString Server::getUptime() const
{
    quint64 secs = m_startTime.secsTo(QDateTime::currentDateTime());

    int hours = secs / 3600;
    int mins = (secs % 3600) / 60;
    int s = secs % 60;
    return QString("%1h %2m %3s").arg(hours).arg(mins).arg(s);
}

bool Server::isValidName(const QString &name)
{
    if(name.length() < 3 || name.length() > 20)  return false;

    if(name.contains(":")) return false;

    if(name.trimmed().isEmpty()) return false;

    return true;
}

void Server::sendToAll(const QString &message)
{
    for(auto *socket : m_clients.values())
    {
        socket->write(message.toUtf8());
    }
    log("Broadcast: " + message);
}

void Server::log(const QString &message, LogLevel level)
{
    QString prefix = "[INFO]";
    if (level == LogLevel::Warning) prefix = "[WARN]";
    if (level == LogLevel::Error)   prefix = "[ERR!]";

    QString time = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    qDebug() << QString("%1 %2 %3").arg(time, prefix, message).toUtf8().constData();
}











