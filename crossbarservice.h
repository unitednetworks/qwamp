#ifndef CROSSBARSERVICE_H
#define CROSSBARSERVICE_H

#include <QObject>
#include <QList>

#include <functional>

#include "autobahn_qt.h"
#include <qvariantmapper.h>

class CrossbarService : public QObject {
    Q_OBJECT

  public:
    CrossbarService();
    ~CrossbarService();

    typedef std::function<QVariant(const QVariantList &, const QVariantMap &, Autobahn::Endpoint::Function)> EndpointWrapper;

    template<class T>
    using ParamConverter = std::function<void(T &, const QVariant &)>;

    typedef std::function<void(void *, const QVariant &)> VoidParamConverter;

    template<class T>
    using ResultConverter = std::function<void(QVariant &, const T &)>;

    typedef std::function<void(QVariant &, const void *)> VoidResultConverter;

    static void registerServices(Autobahn::Session &session);

    static inline const QString &getPrefix() { return m_prefix; }
    static void setPrefix(const QString &s);
    static inline void setWrapper(EndpointWrapper w) { wrapper = w; }
    static inline void setAddClassName(bool b)       { m_addClassName = b; }

    template<class T>
    void registerParamConverter(ParamConverter<T> converter) {
      paramConverters[qMetaTypeId<T>()] = [=](void *v, const QVariant &arg) {
        T *t = (T *)v;
        converter(*t, arg);
      };
      paramConverters[qMetaTypeId<QList<T>>()]  = [=](void *v, const QVariant &args) {
        if (args.type() != QVariant::List) {
          throw std::runtime_error("list converter accepts only list argument");
        }
        QList<T> *listT = (QList<T> *)v;
        for (const QVariant &arg : args.toList()) {
          T t;
          converter(t, arg);
          listT->append(t);
        }
      };
    }

    template<class T>
    void registerResultConverter(ResultConverter<T> converter) {
      resultConverters[qMetaTypeId<T>()] = [=](QVariant &arg, const void *v) {
        const T *t = (const T *)v;
        converter(arg, *t);
      };
      resultConverters[qMetaTypeId<QList<T>>()] = [=](QVariant &res, const void *v) {
        const QList<T> *listT = (const QList<T> *)v;
        QVariantList resList;
        for (const T &t : *listT) {
          QVariant resItem;
          converter(resItem, t);
          resList.append(resItem);
        }
        res = resList;
      };
    }

    template<class T>
    void registerMapper(const QVariantClassMapper<T> *mapper) {
//      int type = qRegisterMetaType<T>();
//      int listType = qRegisterMetaType<QList<T>>();
//      int type = qMetaTypeId<T>();
//      int listType = qMetaTypeId<QList<T>>();
      registerParamConverter<T>([mapper](T &t, const QVariant &v) {
        if (v.type() != QVariant::Map) {
          throw std::runtime_error("mapper converter accepts only QVariantMap");
        }
        t = mapper->mapFromQVariant(v.toMap());
      });
      registerResultConverter<T>([mapper](QVariant &v, const T &t) {
        v = mapper->mapToQVariantMap(t);
      });
    }

  protected:
    void registerBasicParamConverters();

    template<typename T>
    void registerSimpleParamConverter(T (QVariant::*toT)() const) {
      registerParamConverter<T>([=](T &t, const QVariant &arg) {
        if (!arg.canConvert(qMetaTypeId<T>())) {
          throw std::runtime_error("invalid param conversion");
        }
        t = (arg.*toT)();
      });
    }

    template<class T>
    void registerSimpleParamConverter(std::function<T (const QVariant &)> toT) {
      registerParamConverter<T>([=](T &t, const QVariant &arg) {
        if (!arg.canConvert(qMetaTypeId<T>())) {
          throw std::runtime_error("invalid param conversion");
        }
        t = toT(arg);
      });
    }

    static void qTimeParamConverter(QTime &time, const QVariant &);
    static void qTimeResultConverter(QVariant &, const QTime &time);
    static void qDateTimeResultConverter(QVariant &, const QDateTime &dateTime);

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

    VoidParamConverter paramConverter(const QMetaMethod &metaMethod, int i) const;
    static void *convertParameter(const QVariant &arg, int parameterType, VoidParamConverter converter);
    VoidResultConverter resultConverter(int returnType) const;
    static QVariant convertResult(void *result, int returnType, VoidResultConverter converter);

    static QList<CrossbarService*> *services;
    static QString m_prefix;
    static bool m_addClassName;
    static EndpointWrapper wrapper;

    QMap<int, VoidParamConverter> paramConverters;
    QMap<int, VoidResultConverter> resultConverters;
};

#endif // CROSSBARSERVICE_H
