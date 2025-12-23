#ifndef SERVER_H
#define SERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMap>
#include <QDateTime>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlDatabase>

class Server : public QObject {
    Q_OBJECT
public:
    explicit Server(QObject *parent = nullptr);

    enum class LogLevel{
        Info,
        Warning,
        Error
    };

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    QTcpServer *m_server;
    QMap<QString, QTcpSocket*> m_clients;
    QDateTime m_startTime;
    QSqlDatabase m_db;

    void broadcastUserList();
    QString getUptime() const;
    bool isValidName(const QString &name);
    void sendToAll(const QString &message);
    void log(const QString &message,LogLevel level = LogLevel::Info);
    void initDb();
    void sendChatHistory(QTcpSocket *socket,const QString &myNick,const QString &friendNick);
};

#endif
