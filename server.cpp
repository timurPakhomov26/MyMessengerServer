#include "server.h"
#include <QStringList>
#include <QDateTime>

Server::Server(QObject *parent) : QObject(parent)
{
    m_server = new QTcpServer(this);
    m_startTime = QDateTime::currentDateTime();

    initDb();

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

    // QString data = QString::fromUtf8(socket->readAll()).trimmed();
    QByteArray rawData = socket->readAll();
    if (rawData.isEmpty()) return;

    if (rawData.startsWith("FILE:")) {
        handleFileTransfer(socket, rawData);
    } else {
        QString textData = QString::fromUtf8(rawData).trimmed();
        // Если это не пустой мусор - обрабатываем как текст
        if (!textData.isEmpty() && textData.length() < 1000) {
            handleTextMessage(socket, textData);
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
    QByteArray data = (message + "\n").toUtf8();
    for(auto *socket : m_clients.values())
    {
        socket->write(data);
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

void Server::initDb()
{
    m_db = QSqlDatabase::addDatabase("QPSQL");
    m_db.setHostName("localhost");
    m_db.setDatabaseName("messenger_db");
    m_db.setUserName("postgres");
    m_db.setPassword("MY_RRRITK2");

    if (!m_db.open())
    {
        log("Database connection FAILED: " + m_db.lastError().text(), LogLevel::Error);
    } else {
        log("Database connection SUCCESS! PostgreSQL is ready.");
    }
}

void Server::sendChatHistory(QTcpSocket *socket, const QString &myNick, const QString &friendNick) {
    QSqlQuery query;

    // Бронебойный запрос: достаем отправителя, текст(имя файла), время, флаг файла и сами байты
    // Выбираем последние 50 сообщений
    if (myNick == friendNick) {
        query.prepare("SELECT sender, message, timestamp, is_file, file_data FROM messages "
                      "WHERE sender = :me AND receiver = :me "
                      "ORDER BY timestamp ASC LIMIT 50");
    } else {
        query.prepare("SELECT sender, message, timestamp, is_file, file_data FROM messages "
                      "WHERE (sender = :me AND receiver = :friend) "
                      "OR (sender = :friend AND receiver = :me) "
                      "ORDER BY timestamp ASC LIMIT 50");
        query.bindValue(":friend", friendNick);
    }
    query.bindValue(":me", myNick);

    if (query.exec()) {
        while (query.next()) {
            QString sender = query.value(0).toString();
            QString messageText = query.value(1).toString();
            QString time = query.value(2).toDateTime().toString("hh:mm");
            bool isFile = query.value(3).toBool();

            if (isFile) {
                // Если это файл — достаем байты из BYTEA
                QByteArray fileBytes = query.value(4).toByteArray();

                // Формируем наш стандартный пакет FILE_REC:от_кого:имя:размер:байты
                QByteArray filePacket = "FILE_REC:" + sender.toUtf8() + ":" +
                                        messageText.toUtf8() + ":" +
                                        QByteArray::number(fileBytes.size()) + ":" +
                                        fileBytes;

                socket->write(filePacket);
                // Небольшая задержка, чтобы TCP не склеил два файла в один поток (на всякий случай)
                socket->waitForBytesWritten(100);
            } else {
                // Если это обычный текст — шлем по старому протоколу
                QString line = QString("%1 %2: %3\n").arg(time, sender, messageText);
                socket->write(line.toUtf8());
            }
        }
        log("History (with media) sent to " + myNick + " for chat with " + friendNick);
    } else {
        log("SQL History Error: " + query.lastError().text(), LogLevel::Error);
    }
}

void Server::handleFileTransfer(QTcpSocket *socket, const QByteArray &data)
{
    QByteArray fullData = data;

    // 1. Быстро парсим заголовок, чтобы узнать, сколько байт МЫ ЖДЕМ
    QList<QByteArray> parts = fullData.split(':');
    if (parts.size() < 4) return;

    int expectedSize = parts[3].toInt(); // Тот размер, который прислал клиент
    QString fileName = QString::fromUtf8(parts[2]);
    QString target = QString::fromUtf8(parts[1]);

    // 2. ДОЧИТЫВАЕМ ИЗ СОКЕТА, пока не наберем нужный вес
    // Мы даем серверу немного подождать (блокирующее чтение для простоты)
    while (fullData.size() < (expectedSize + 50)) { // +50 на заголовок
        if (socket->waitForReadyRead(500)) { // Ждем данные полсекунды
            fullData.append(socket->readAll());
        } else {
            break; // Тайм-аут
        }
    }

    // 3. Теперь, когда у нас (надеюсь) всё в сборе, вырезаем байты
    int headerSize = 5 + parts[1].size() + 1 + parts[2].size() + 1 + parts[3].size() + 1;
    QByteArray fileBytes = fullData.mid(headerSize, expectedSize);

    QString senderName = m_clients.key(socket);

    // 4. СОХРАНЯЕМ В БД (только если реально долетело много байт)
    QSqlQuery query;
    query.prepare("INSERT INTO messages (sender, receiver, message, file_data, is_file) VALUES (:s, :r, :m, :d, TRUE)");
    query.bindValue(":s", senderName);
    query.bindValue(":r", target);
    query.bindValue(":m", fileName);
    query.bindValue(":d", fileBytes);

    if (query.exec()) {
        log(QString("SUCCESS: File %1 (%2 bytes) saved from %3").arg(fileName).arg(fileBytes.size()).arg(senderName));

        // 5. РАССЫЛКА КЛИЕНТАМ (уже с новым заголовком FILE_REC)
        QByteArray relayPacket = "FILE_REC:" + senderName.toUtf8() + ":" + fileName.toUtf8() + ":" +
                                 QByteArray::number(fileBytes.size()) + ":" + fileBytes;

        if (m_clients.contains(target)) m_clients[target]->write(relayPacket);
        socket->write(relayPacket);
    }
}

void Server::handleTextMessage(QTcpSocket *socket, const QString &data)
{
    if (data.isEmpty()) return;

    // 1.1. РЕГИСТРАЦИЯ (Если юзер еще не в системе)
    if (!m_clients.values().contains(socket)) {
        if (isValidName(data)) {
            m_clients[data] = socket;

            // СНАЧАЛА добавили в QMap, ПОТОМ рассылаем всем
            sendToAll("SYSTEM: Пользователь [" + data + "] вошел в чат");
            broadcastUserList(); // Тот самый метод, который оживит твой список справа

            log("User registered: " + data);
        } else {
            log("Rejected nick: " + data, LogLevel::Warning);
            socket->write("SYSTEM: Invalid nickname!\n");
            socket->disconnectFromHost();
        }
        return; // ОБЯЗАТЕЛЬНО выходим
    }


    // 1.2. КОМАНДЫ (Уже для зарегистрированных)
    if (data.startsWith("/get_history ")) {
        QString friendNick = data.mid(13).trimmed();
        QString myNick = m_clients.key(socket);
        sendChatHistory(socket, myNick, friendNick);
        return;
    }

    if (data == "/uptime") {
        socket->write(QString("SERVER: My uptime is %1\n").arg(getUptime()).toUtf8());
        return;
    }

    // 1.3. ПЕРЕСЫЛКА ЛИЧНЫХ СООБЩЕНИЙ
    if (data.contains(":")) {
        QString target = data.section(':', 0, 0);
        QString text = data.section(':', 1);
        QString senderName = m_clients.key(socket);

        if (target.length() > 20  || target.contains(" "))
            return;

        if (m_clients.contains(target))
        {
            QString time = QDateTime::currentDateTime().toString("hh:mm");
            QString packet = QString("%1 %2: %3\n").arg(time, senderName, text);

            m_clients[target]->write(packet.toUtf8());
            if (target != senderName) {
                socket->write(packet.toUtf8());
            }

            // Запись в БД (is_file = FALSE)
            QSqlQuery query;
            query.prepare("INSERT INTO messages (sender, receiver, message, is_file) "
                          "VALUES (:s, :r, :m, FALSE)");
            query.bindValue(":s", senderName);
            query.bindValue(":r", target);
            query.bindValue(":m", text);
            query.exec();
        }
        else
        {
            socket->write("SYSTEM: User not found.\n");
        }

    }
    }












