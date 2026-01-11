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

    if (rawData.startsWith("VOICE_DATA:")) {
        // Рассылаем звук ВСЕМ, кто сейчас помечен в базе как in_voice = TRUE
        // (Для скорости лучше кэшировать список участников в памяти QSet<QTcpSocket*>)
        for (QTcpSocket *s : m_voiceParticipants) {
            if (s != socket) {
                s->write(rawData);
                s->flush();
            }
        }
        return;
    }
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
    m_voiceParticipants.remove(socket);
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

    // Узнаем, кто нам пишет (сокет должен быть в m_clients после AUTH)
    QString senderName = m_clients.key(socket, "");

    if (senderName.isEmpty()) {
        socket->write("SYSTEM: Сначала авторизуйтесь (AUTH:login:pass)\n");
        return;
    }

    // --- 1. ОБРАБОТКА КОМАНД ---
    if (data.startsWith("/get_history ")) {
        QString target = data.mid(13).trimmed();
        if (target == "GROUP_CHAT") {
            log("Запрос истории ОБЩАКА от: " + senderName);
            sendGroupHistory(socket);
        } else {
            log("Запрос личной истории: " + senderName + " <-> " + target);
            sendChatHistory(socket, senderName, target);
        }
        return;
    }
    //------------voice channel----------------
    if (data == "/voice_enter") {
        m_voiceParticipants.insert(socket);
        QSqlQuery().exec(QString("UPDATE users SET in_online = TRUE WHERE username = '%1'").arg(senderName));
        socket->write("SYSTEM: Ты вошел в голосовой канал\n");
        return;
    }

    if (data == "/voice_leave") {
        m_voiceParticipants.remove(socket);
        QSqlQuery().exec(QString("UPDATE users SET in_online = FALSE WHERE username = '%1'").arg(senderName));
        socket->write("SYSTEM: Ты покинул голосовой канал\n");
        return;
    }

    // --- 2. ОБРАБОТКА СООБЩЕНИЙ (ЛИЧНЫХ И ГРУППОВЫХ) ---
    if (data.contains(":")) {
        QString target = data.section(':', 0, 0).trimmed();
        QString text = data.section(':', 1).trimmed();
        QString time = QDateTime::currentDateTime().toString("hh:mm");

        // А. ОБЩИЙ ЧАТ (GROUP_CHAT)
        if (target == "GROUP_CHAT") {
            // Сохраняем в таблицу group_messages
            QSqlQuery q;
            QString sql = QString("INSERT INTO group_messages (sender, message) VALUES ('%1', '%2')")
                              .arg(senderName).arg(text);
            q.exec(sql);

            // Рассылаем ВСЕМ онлайн
            QString packet = QString("GROUP_MSG:%1:%2:%3\n").arg(time, senderName, text);
            for (QTcpSocket *s : m_clients.values()) {
                s->write(packet.toUtf8());
                s->flush();
            }
            log("Групповое сообщение от [" + senderName + "]: " + text);
            return;
        }

        // Б. ЛИЧНЫЕ СООБЩЕНИЯ
        if (m_clients.contains(target)) {
            // Если адресат онлайн - шлем сразу
            QString packet = QString("%1 %2: %3\n").arg(time, senderName, text);
            m_clients[target]->write(packet.toUtf8());
            m_clients[target]->flush();

            // Себе тоже шлем, чтобы отобразилось в окне
            socket->write(packet.toUtf8());
            socket->flush();
        } else {
            socket->write(("SYSTEM: Пользователь [" + target + "] сейчас оффлайн. Сообщение сохранено.\n").toUtf8());
        }

        // В любом случае сохраняем личку в таблицу messages
        QSqlQuery q;
        QString sql = QString("INSERT INTO messages (sender, receiver, message, is_file) "
                              "VALUES ('%1', '%2', '%3', FALSE)")
                          .arg(senderName).arg(target).arg(text);
        q.exec(sql);
    }
}

void Server::sendGroupHistory(QTcpSocket *socket)
{
    QSqlQuery q;
    // Прямой запрос без всяких prepare
    if (q.exec("SELECT sender, message, timestamp FROM group_messages ORDER BY timestamp ASC LIMIT 50")) {
        int count = 0;
        while (q.next()) {
            count++;
            QString packet = QString("GROUP_MSG:%1:%2:%3\n")
                                 .arg(q.value(2).toDateTime().toString("hh:mm"),
                                      q.value(0).toString(),
                                      q.value(1).toString());
            socket->write(packet.toUtf8());
        }
        socket->flush();
        log("ОТПРАВЛЕНО СТРОК ИСТОРИИ: " + QString::number(count)); // Проверь это в логе!
    } else {
        log("ОШИБКА SQL: " + q.lastError().text());
    }
}












