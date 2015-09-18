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
    typedef std::function<void*(const QVariant &)> ParamConverter;

    static void registerServices(Autobahn::Session &session);

    static inline const QString &getPrefix() { return m_prefix; }
    static void setPrefix(const QString &s);
    static inline void setWrapper(EndpointWrapper w) { wrapper = w; }
    static inline void setAddClassName(bool b)       { m_addClassName = b; }

    void registerParamConverter(int type, ParamConverter converter);

  protected:
    void registerBasicParamConverters();

    template<typename T>
    void registerSimpleParamConverter(QMetaType::Type type, T (QVariant::*toT)() const) {
      registerParamConverter(type, [=](const QVariant &v)->void* {
        if (v.canConvert(type)) {
          return new T((v.*toT)());
        }
        return 0;
      });
    }

    template<class R>
    void registerSimpleParamConverter(QMetaType::Type type, R toT) {
      registerParamConverter(type, [=](const QVariant &v)->void* {
        if (v.canConvert(type)) {
          return new decltype(toT(v)) (toT(v));
        }
        return 0;
      });
    }

    static void *qTimeConverter(const QVariant &);

    QString apiClassName;

  private:
    struct Cleaner {
      typedef std::function<void()> CleanerFunction;
      inline Cleaner(CleanerFunction f):cleanerFunction(f) {}
      ~Cleaner() {
        cleanerFunction();
      }
      CleanerFunction cleanerFunction;
    };

    static QList<CrossbarService*> *services;
    static QString m_prefix;
    static bool m_addClassName;
    static EndpointWrapper wrapper;
    QMap<int, ParamConverter> paramConverters;
};

#endif // CROSSBARSERVICE_H

