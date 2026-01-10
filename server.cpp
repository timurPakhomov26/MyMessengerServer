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
    QSqlQuery query("SELECT username, is_online FROM users ORDER BY username ASC");
    QStringList userStatusList;

    while (query.next()) {
        QString user = query.value(0).toString();
        bool isOnline = query.value(1).toBool();
        // Формат пакета: timur:1,vovchik:0
        userStatusList << QString("%1:%2").arg(user).arg(isOnline ? "1" : "0");
    }

    QString packet = "USERS_LIST:" + userStatusList.join(",") + "\n";

    // Рассылаем этот список ВСЕМ, кто сейчас физически в сети
    for (QTcpSocket *socket : m_clients.values()) {
        socket->write(packet.toUtf8());
    }
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

    QByteArray rawData = socket->readAll();
    log("ПОЛУЧЕНО СЫРЫХ БАЙТ: " + QString::number(rawData.size()) + " Содержимое: " + rawData);

    // QString data = QString::fromUtf8(socket->readAll()).trimmed();
    if (rawData.isEmpty()) return;

    auto senderName = m_clients.key(socket, "");
    bool isAuthenticated = !senderName.isEmpty();

    if (rawData.startsWith("FILE:")) {
        if (!isAuthenticated) {
            log("Попытка кражи трафика! Неавторизованный файл отброшен.", LogLevel::Warning);
            socket->write("SYSTEM: Сначала авторизуйтесь!\n");
            return;
        }
        handleFileTransfer(socket, rawData);
        return;
    }
    QString data = QString::fromUtf8(rawData).trimmed();

    // А. Блок авторизации (Сюда пускаем ДАЖЕ неавторизованных)
    if (data.startsWith("AUTH:")) {
        QStringList parts = data.split(':');
        if (parts.size() >= 3) {
            QString user = parts[1].trimmed();
            QString pass = parts[2].split('\n')[0].trimmed();

            QSqlQuery checkQuery;
            QString checkStr = QString("SELECT password_hash FROM users WHERE username = '%1'").arg(user);

            if (checkQuery.exec(checkStr) && checkQuery.next()) {
                // --- 1. ЮЗЕР СУЩЕСТВУЕТ -> ПРОВЕРЯЕМ ПАРОЛЬ ---
                QString dbPass = checkQuery.value(0).toString();
                if (dbPass == pass) {
                    m_clients[user] = socket;
                    socket->write("AUTH_OK\n");

                    // Ставим статус Online
                    QSqlQuery up;
                    up.exec(QString("UPDATE users SET is_online = TRUE WHERE username = '%1'").arg(user));

                    broadcastUserList();
                    log("SUCCESS: " + user + " вошел в систему (пароль верный)");
                } else {
                    socket->write("AUTH_FAIL:Неверный пароль!\n");
                    log("FAIL: Неверный пароль для юзера " + user);
                }
            } else {
                // --- 2. ЮЗЕРА НЕТ -> РЕГИСТРИРУЕМ НОВОГО ---
                QSqlQuery regQuery;
                QString regStr = QString("INSERT INTO users (username, password_hash, is_online) "
                                         "VALUES ('%1', '%2', TRUE)").arg(user).arg(pass);

                if (regQuery.exec(regStr)) {
                    m_clients[user] = socket;
                    socket->write("AUTH_OK:Registered\n");
                    broadcastUserList();
                    log("SUCCESS: Новый юзер " + user + " создан в БД");
                } else {
                    log("КРИТИЧЕСКАЯ ОШИБКА РЕГИСТРАЦИИ: " + regQuery.lastError().text(), LogLevel::Error);
                }
            }
        }
        return;
    }

    if (!isAuthenticated) {
        socket->write("SYSTEM: Доступ запрещен. Введите логин и пароль.\n");
        return;
    }

    handleTextMessage(socket, data);

}

void Server::onDisconnected()
{
    auto *socket = qobject_cast<QTcpSocket*>(sender());
    QString name = m_clients.key(socket,"");
    if (!name.isEmpty())
    {
        m_clients.remove(name);
        QSqlQuery query;
        query.prepare("UPDATE users SET is_online = FALSE, last_seen = NOW() WHERE username = :u");
        query.bindValue(":u", name);
        query.exec();

        log("User offline: " + name);
        broadcastUserList();
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
    QString text = data.section(':', 1).trimmed();
    // Имя отправителя (уже залогиненного через AUTH)
    QString senderName = m_clients.key(socket, "");
    if (senderName.isEmpty()) return;

    // --- 1.2. КОМАНДЫ ---
    if (data.startsWith("/get_history ")) {
        QString target = data.mid(13).trimmed();

        if (target == "GROUP_CHAT") {

            //sendGroupHistory(socket);
            QSqlQuery q;
            // Прямой INSERT без prepare
            QString sql = QString("INSERT INTO group_messages (sender, message) VALUES ('%1', '%2')").arg(senderName, text);
            if (!q.exec(sql)) {
                log("ОШИБКА ЗАПИСИ В ОБЩАК: " + q.lastError().text());
            }

            QString time = QDateTime::currentDateTime().toString("hh:mm");
            QString packet = QString("GROUP_MSG:%1:%2:%3\n").arg(time, senderName, text);

            for (QTcpSocket *s : m_clients.values()) {
                s->write(packet.toUtf8());
                s->flush(); // Выталкиваем байты!
            }
            return;
        } else {
            // Отправляем личную историю
            sendChatHistory(socket, senderName, target);
        }
        return;
    }

    if (data == "/uptime") {
        socket->write(QString("SERVER: My uptime is %1\n").arg(getUptime()).toUtf8());
        return;
    }

    // --- 1.3. ПЕРЕСЫЛКА СООБЩЕНИЙ (ЛИЧНЫХ И ГРУППОВЫХ) ---
    if (data.contains(":")) {
        QString target = data.section(':', 0, 0).trimmed();


        // А. ЛОГИКА ОБЩЕГО ЧАТА
        if (target == "GROUP_CHAT") {
            // 1. Сохраняем в таблицу group_messages
            QSqlQuery query;
            QString insertSql = QString("INSERT INTO group_messages (sender, message) VALUES ('%1', '%2')")
                                    .arg(senderName).arg(text);
            query.exec(insertSql);

            // 2. Рассылаем ВСЕМ онлайн (включая себя)
            QString time = QDateTime::currentDateTime().toString("hh:mm");
            // Шлем спец-пакет GROUP_MSG, чтобы клиент понял, что это Общак
            QString packet = QString("GROUP_MSG:%1:%2:%3\n").arg(time, senderName, text);

            for (QTcpSocket *s : m_clients.values()) {
                s->write(packet.toUtf8());
            }
            log("Групповое сообщение от: " + senderName);
            return;
        }

        if (m_clients.contains(target))
        {
            QString time = QDateTime::currentDateTime().toString("hh:mm");
            QString packet = QString("%1 %2: %3\n").arg(time, senderName, text);

            m_clients[target]->write(packet.toUtf8());
            if (target != senderName) {
                socket->write(packet.toUtf8());
            }

            // Запись в БД личных сообщений
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

void Server::sendGroupHistory(QTcpSocket *socket)
{
    // QSqlQuery q;
    // // Прямой INSERT без prepare
    // QString sql = QString("INSERT INTO group_messages (sender, message) VALUES ('%1', '%2')").arg(senderName, text);
    // if (!q.exec(sql)) {
    //     log("ОШИБКА ЗАПИСИ В ОБЩАК: " + q.lastError().text());
    // }

    // QString time = QDateTime::currentDateTime().toString("hh:mm");
    // QString packet = QString("GROUP_MSG:%1:%2:%3\n").arg(time, senderName, text);

    // for (QTcpSocket *s : m_clients.values()) {
    //     s->write(packet.toUtf8());
    //     s->flush(); // Выталкиваем байты!
    // }
    // return;
}












