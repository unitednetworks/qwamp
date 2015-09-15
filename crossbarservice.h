#ifndef CROSSBARSERVICE_H
#define CROSSBARSERVICE_H

#include <QObject>

#include <functional>

#include "autobahn_qt.h"

class CrossbarService : public QObject {
    Q_OBJECT

  public:
    CrossbarService();
    ~CrossbarService();

    typedef std::function<QVariant(const QVariantList &, const QVariantMap &, Autobahn::Endpoint::Function)> EndpointWrapper;

    static void registerServices(Autobahn::Session &session);

    static inline const QString &getPrefix() { return m_prefix; }
    static void setPrefix(const QString &s);
    static inline void setWrapper(EndpointWrapper w) { wrapper = w; }
    static inline void setAddClassName(bool b)       { m_addClassName = b; }

  protected:
    QString apiClassName;

  private:
    static QList<CrossbarService*> *services;
    static QString m_prefix;
    static bool m_addClassName;
    static EndpointWrapper wrapper;
};

#endif // CROSSBARSERVICE_H

