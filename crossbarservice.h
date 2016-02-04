#ifndef CROSSBARSERVICE_H
#define CROSSBARSERVICE_H

#include <QObject>
#include <QList>

#include <functional>

#include "autobahn_qt.h"
#include <qvariantmapper.h>

template<class T>
void enumConverter(T &t, const QVariant &v) {
  if (v.type() != QVariant::String && v.type() != QVariant::Int && v.type() != QVariant::UInt && v.type() != QVariant::ULongLong) {
    throw std::runtime_error("Enum argument must be passed as string or int");
  }
  QMetaEnum enumerator = QMetaEnum::fromType<T>();
  int val;
  if (v.type() == QVariant::String) {
    QString vs = v.toString();
    if (vs.isEmpty()) {
      throw std::runtime_error("String enum argument must not be empty");
    }
    val = enumerator.keyToValue(vs.toUtf8().constData());
    if (val == -1) {
      throw std::runtime_error("illegal enum value " + vs.toStdString() + " in enum " + enumerator.scope() + "::" + enumerator.name());
    }
  }
  else {
    val = v.toInt();
    if (!enumerator.valueToKey(val)) {
      throw std::runtime_error("illegal enum value " + QString::number(val).toStdString() + " in enum " + enumerator.scope() + "::" + enumerator.name());
    }
  }
  t = T(val);
}

//template<class T>
//void registerEnumConverter() {

//  QMetaType::registerConverter<QVariant, T>([](const QVariant &v) {
//    return convertToEnum<T>(v);
//  });

//  QMetaType::registerConverter<QVariant, QList<T>>([convertToEnum](const QVariant &v) {
//    QList<T> enums;
//    if (v.type() != QVariant::List) {
//      throw std::runtime_error("array enum argument must be passed as list");
//    }
//    QVariantList l = v.toList();
//    for (const QVariant &lv : l) {
//      enums.append(convertToEnum<T>(lv));
//    }
//    return enums;
//  });
//}

class CrossbarService : public QObject {
    Q_OBJECT

  public:
    ~CrossbarService();

    template<class T>
    using ParamConverter = std::function<void(T &, const QVariant &)>;

    typedef std::function<void(void *, const QVariant &)> VoidParamConverter;

    template<class T>
    using ResultConverter = std::function<void(QVariant &, const T &)>;

    typedef std::function<void(QVariant &, const void *)> VoidResultConverter;

    static void registerServices(Autobahn::Session &session);

    static inline const QString &getPrefix() { return m_prefix; }
    static void setPrefix(const QString &s);
    static inline void setCommonWrapper(Autobahn::EndpointWrapper w) { commonWrapper = w; }
    static inline void setAddClassName(bool b)       { m_addClassName = b; }

    template<class T>
    void registerParamConverter(ParamConverter<T> converter) {
      this->registerParamConverter(converter, paramConverters);
    }

    template<class T>
    static void registerStaticParamConverter(ParamConverter<T> converter) {
      registerParamConverter(converter, staticParamConverters);
    }

    template<class T>
    void registerEnumConverter() {
      registerParamConverter(ParamConverter<T>(enumConverter<T>));
    }

    template<class T>
    static void registerStaticEnumConverter() {
      registerStaticParamConverter(ParamConverter<T>(enumConverter<T>));
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
//      qRegisterMetaType<T>();
//      qRegisterMetaType<QList<T>>();
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
    CrossbarService(Autobahn::Endpoint::Type callType = Autobahn::Endpoint::Sync);
    void registerBasicParamConverters();

    template<class T>
    static void registerParamConverter(ParamConverter<T> converter, QMap<int, VoidParamConverter> &paramConverters) {
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

    void addWrapper(Autobahn::EndpointWrapper wrapper);

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
    static Autobahn::EndpointWrapper commonWrapper;

    QMap<int, VoidParamConverter> paramConverters;
    QMap<int, VoidResultConverter> resultConverters;
    static QMap<int, VoidParamConverter> staticParamConverters;
    static QMap<int, VoidResultConverter> staticResultConverters;
    QList<Autobahn::EndpointWrapper> wrappers;

    Autobahn::Endpoint::Type callType;
};

template<typename T>
class CrossbarEnumRegistrator {
};

#define CROSSBAR_ENUM(TYPE) CROSSBAR_ENUM_IMPL(TYPE)
#define CROSSBAR_ENUM_IMPL(TYPE)   \
  template<> \
  class CrossbarEnumRegistrator<TYPE> { \
    public:                                                        \
      CrossbarEnumRegistrator() {                                  \
        CrossbarService::registerStaticEnumConverter<TYPE>();      \
      }                                                            \
      static CrossbarEnumRegistrator<TYPE> registrator;            \
  };                                                               \
                                                                   \
CrossbarEnumRegistrator<TYPE> CrossbarEnumRegistrator<TYPE>::registrator;


#endif // CROSSBARSERVICE_H
