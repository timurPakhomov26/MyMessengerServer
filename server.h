#ifndef SERVER_H
#define SERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMap>
#include <QDateTime>

class Server : public QObject {
    Q_OBJECT
public:
    explicit Server(QObject *parent = nullptr);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    QTcpServer *m_server;
    QMap<QString, QTcpSocket*> m_clients;
    void broadcastUserList();
    QDateTime m_startTime;
    QString getUptime() const;
    bool isValidName(const QString &name);
};

#endif
