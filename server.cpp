#include "server.h"
#include <QStringList>
#include <QDateTime>

Server::Server(QObject *parent) : QObject(parent) {
    m_server = new QTcpServer(this);
    m_startTime = QDateTime::currentDateTime();

    connect(m_server, &QTcpServer::newConnection, this, &Server::onNewConnection);

    // Слушаем порт 1234
    if (m_server->listen(QHostAddress::Any, 1234)) {
        qDebug() << "--- SERVER STARTED ON PORT 1234 ---";
    } else {
        qDebug() << "--- SERVER ERROR: " << m_server->errorString();
    }
}

// Функция рассылки списка имен всем подключенным
void Server::broadcastUserList() {
    QStringList userList = m_clients.keys();
    // Формируем спец-сообщение с префиксом USERS_LIST:
    QByteArray listData = ("USERS_LIST:" + userList.join(",")).toUtf8();

    for (QTcpSocket *socket : m_clients.values()) {
        socket->write(listData);
    }
    qDebug() << "Broadcasted user list:" << userList.join(",");
}

void Server::onNewConnection() {
    QTcpSocket *socket = m_server->nextPendingConnection();
    connect(socket, &QTcpSocket::readyRead, this, &Server::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &Server::onDisconnected);
    qDebug() << "New connection attempt...";
}

void Server::onReadyRead() {
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    QString data = QString::fromUtf8(socket->readAll()).trimmed();
    if (data.isEmpty()) return;

    if (data == "/uptime") {
        socket->write(QString("SERVER: My uptime is %1").arg(getUptime()).toUtf8());
        return;
    }
    if(data.startsWith("/me ")){
        QString sender = m_clients.key(socket);
        QString str = data.mid(4);
        QString broadcastMsg = "* " + sender + " " + str;

        for (auto *clientSocket : m_clients.values()) {
            clientSocket->write(broadcastMsg.toUtf8());
        }

        return;

    }
    // 1. РЕГИСТРАЦИЯ (если сокета еще нет в карте)
    if (!m_clients.values().contains(socket)) {
        m_clients[data] = socket;
        qDebug() << "User registered:" << data;       
        // СРАЗУ рассылаем обновленный список всем
        broadcastUserList();
        return;
    }

    // 2. ПЕРЕСЫЛКА СООБЩЕНИЯ (Формат "Кому:Текст")
    if (data.contains(":")) {
        QString target = data.section(':', 0, 0);
        QString text = data.section(':', 1);
        QString senderName = m_clients.key(socket);

        if (m_clients.contains(target)) {
            m_clients[target]->write(QString("%1: %2").arg(senderName, text).toUtf8());
        } else {
            socket->write("System: User not found.");
        }
    }

    if (data.startsWith("/")) {
        if (data == "/uptime") {
            // Вызываем наш метод и отправляем ответ именно этому сокету
            socket->write(QString("SERVER: Мой аптайм уже %1")
                              .arg(getUptime()).toUtf8());
        }
        else if (data == "/help") {
            socket->write("SERVER: Доступные команды: /uptime, /help");
        }
        return;
    }
}

void Server::onDisconnected() {
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    QString name = m_clients.key(socket);
    m_clients.remove(name);
    socket->deleteLater();

    qDebug() << "User disconnected:" << name;

    // СРАЗУ рассылаем обновленный список (без ушедшего юзера)
    broadcastUserList();
}

QString Server::getUptime() const{
    quint64 secs = m_startTime.secsTo(QDateTime::currentDateTime());

    int hours = secs / 3600;
    int mins = (secs % 3600) / 60;
    int s = secs % 60;
    return QString("%1h %2m %3s").arg(hours).arg(mins).arg(s);
}









