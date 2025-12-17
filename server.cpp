#include "server.h"
#include <QStringList>

Server::Server(QObject *parent) : QObject(parent) {
    m_server = new QTcpServer(this);
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
