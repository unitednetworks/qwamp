#include "crossbarservice.h"

#include <QDebug>
#include <QMetaMethod>
#include <QTime>
#include <QVariant>

#include <exception>
#include <functional>

QList<CrossbarService*> *CrossbarService::services = 0;
QString CrossbarService::m_prefix;
bool CrossbarService::m_addClassName = true;
QWamp::EndpointWrapper CrossbarService::commonWrapper;
QMap<int, CrossbarService::VoidParamConverter> CrossbarService::staticParamConverters;
QMap<int, CrossbarService::VoidResultConverter> CrossbarService::staticResultConverters;

CrossbarService::CrossbarService(QWamp::Endpoint::Type callType) : callType(callType) {
  if (!services) {
    services = new QList<CrossbarService*>;
  }
  services->append(this);

  registerBasicParamConverters();
}

CrossbarService::~CrossbarService() {
  services->removeOne(this);
  if (services->count() == 0) {
    delete services;
  }
}

void CrossbarService::setPrefix(const QString &s) {
  m_prefix = s;
}

void CrossbarService::registerBasicParamConverters() {
  registerSimpleParamConverter<QString>(&QVariant::toString);
  registerSimpleParamConverter<QByteArray>(&QVariant::toByteArray);
  registerSimpleParamConverter<bool>(&QVariant::toBool);
  registerSimpleParamConverter<int>([](const QVariant &v) { return v.toInt(); });
  registerParamConverter<QTime>(qTimeParamConverter);

  registerResultConverter<QTime>(qTimeResultConverter);
  registerResultConverter<QDateTime>(qDateTimeResultConverter);
}

void CrossbarService::qTimeParamConverter(QTime &time, const QVariant &v) {
  if (v.isNull()) {
    time = QTime();
  }
  else {
    if (v.canConvert(QMetaType::QString)) {
      time = QTime::fromString(v.toString(), "HH:mm");
      if (!time.isValid()) {
        throw std::runtime_error("Invalid conversion to QTime");
      }
    }
    else {
      throw std::runtime_error("Bad qtime parameter");
    }
  }
}

void CrossbarService::qTimeResultConverter(QVariant &res, const QTime &time) {
  res = QVariant(time.toString("HH:mm"));
}

void CrossbarService::qDateTimeResultConverter(QVariant &res, const QDateTime &dateTime) {
  res = QVariant(dateTime.toString(Qt::DateFormat::ISODate));
}

void CrossbarService::addWrapper(QWamp::EndpointWrapper wrapper) {
  wrappers.append(wrapper);
}

CrossbarService::VoidParamConverter CrossbarService::paramConverter(const QMetaMethod &metaMethod, int i) const {
  int parameterType = metaMethod.parameterType(i);
  if (paramConverters.contains(parameterType)) {
    return paramConverters[parameterType];
  }
  else if (staticParamConverters.contains(parameterType)) {
    return staticParamConverters[parameterType];
  }
  else if (QMetaType::hasRegisteredConverterFunction(qMetaTypeId<QVariant>(), parameterType)){
    return [parameterType](void *v, const QVariant &arg) {
      QMetaType::convert(&arg, qMetaTypeId<QVariant>(), v, parameterType);
    };
  }
  else if (QMetaType::typeFlags(parameterType) & QMetaType::IsEnumeration) {
    const QMetaObject *mo = QMetaType::metaObjectForType(parameterType);
    QString pt = metaMethod.parameterTypes()[i];
    pt = pt.mid(pt.lastIndexOf(':') + 1);
    int ei = mo->indexOfEnumerator(pt.toUtf8().constData());
    if (ei == -1) {
      throw std::runtime_error(QString("can't find enum " + pt).toStdString());
    }
    QMetaEnum enumerator = mo->enumerator(ei);
    return [enumerator](void *t, const QVariant &v) {
      if (v.type() != QVariant::String && v.type() != QVariant::Int && v.type() != QVariant::UInt && v.type() != QVariant::ULongLong) {
        throw std::runtime_error("Enum argument must be passed as string or int");
      }
      int *enumValue = (int *)t;
      if (v.type() == QVariant::String) {
        QString vs = v.toString();
        if (vs.isEmpty()) {
          throw std::runtime_error("String enum argument must not be empty");
        }
        int val = enumerator.keyToValue(vs.toUtf8().constData());
        if (val == -1) {
          throw std::runtime_error("illegal enum value " + vs.toStdString() + " in enum " + enumerator.scope() + "::" + enumerator.name());
        }
        *enumValue = enumerator.keyToValue(vs.toUtf8().constData());
      }
      else {
        *enumValue = v.toInt();
      }
    };
  }
  else {
    return 0;
  }
}

class no_converter_error : public std::runtime_error {
  public:
    inline no_converter_error() : std::runtime_error(std::string()) {}
};

class QEVariant : public QVariant {
  public:
    inline bool convert(const int type, void *ptr) const { return QVariant::convert(type, ptr); }
};

void *CrossbarService::convertParameter(const QVariant &arg, int parameterType, VoidParamConverter converter) {
  void *v = QMetaType::create(parameterType);
  try {
    if (arg.isValid()) {
      if (converter) {
        converter(v, arg);
      }
      else if (arg.canConvert(parameterType)) {
        ((const QEVariant &)arg).convert(parameterType, v);
      }
      else {
        throw no_converter_error();
      }
    }
  }
  catch(...) {
    QMetaType::destroy(parameterType, v);
    throw;
  }
  return v;
}

CrossbarService::VoidResultConverter CrossbarService::resultConverter(int returnType) const {
  if (resultConverters.contains(returnType)) {
    return resultConverters[returnType];
  }
  else if (QMetaType::hasRegisteredConverterFunction(returnType, qMetaTypeId<QVariant>())){
    return [returnType](QVariant &res, const void *v) {
      QMetaType::convert(v, returnType, &res, qMetaTypeId<QVariant>());
    };
  }
  else if (returnType >= QMetaType::User && QMetaType::hasRegisteredConverterFunction(returnType, qMetaTypeId<QVariantMap>())) {
    return [returnType](QVariant &res, const void *v) {
      QVariantMap map;
      QMetaType::convert(v, returnType, &map, qMetaTypeId<QVariantMap>());
      res = QVariant(map);
    };
  }
  else if (returnType >= QMetaType::User && QMetaType::hasRegisteredConverterFunction(returnType, qMetaTypeId<QVariantList>())) {
    return [returnType](QVariant &res, const void *v) {
      QVariantList list;
      QMetaType::convert(v, returnType, &list, qMetaTypeId<QVariantList>());
      res = QVariant(list);
    };
  }
  else {
    return 0;
  }
}

QVariant CrossbarService::convertResult(void *result, int returnType, VoidResultConverter converter) {
  QVariant res;
  if (returnType != QMetaType::Void) {
    if (converter) {
      converter(res, result);
    }
    else {
      res = QVariant(returnType, result);
    }
  }
  return res;
}

void CrossbarService::registerServices(QWamp::Session &session) {
  if (!services) {
    return;
  }
  for (CrossbarService *service : *services) {
    const QMetaObject *metaObject = service->metaObject();
    QMap<QString, QList<int>> methods;
    for (int methodOffset = metaObject->methodOffset(); methodOffset < metaObject->methodCount(); ++methodOffset) {
      QMetaMethod metaMethod = metaObject->method(methodOffset);
      if (metaMethod.access() != QMetaMethod::Public) {
        continue;
      }
      methods[metaMethod.name()].append(methodOffset);
    }
    for (auto methodIterator = methods.constBegin(); methodIterator != methods.constEnd(); ++methodIterator) {

      QStringList nameParts;
      if (!m_prefix.isEmpty()) {
        nameParts << m_prefix;
      }
      if (m_addClassName) {
        if (service->apiClassName.isEmpty()) {
          nameParts << metaObject->className();
        }
        else {
          nameParts << service->apiClassName;
        }
      }
      nameParts << methodIterator.key();
      QString crossbarMethodName = nameParts.join(".");
      qDebug() << "Registering" << (session.name().isEmpty() ? crossbarMethodName : session.name() + "." + crossbarMethodName) << qPrintable(methodIterator.value().count() > 1 ? ("(" + QString::number(methodIterator.value().count()) + "x)") : QString());

      struct Method {
          QMetaMethod metaMethod;
          int methodOffset;
          int paramCount;
          int returnType;
          QList<VoidParamConverter> paramConverters;
          VoidResultConverter resultConverter;
      };

      QMap<int, Method> methodList;

      int methodCount = methodIterator.value().count();

      for (int k = 0; k < methodCount; ++k) {
        QMetaMethod metaMethod = metaObject->method(methodIterator.value()[k]);

        Method method;
        method.methodOffset = methodIterator.value()[k];
        method.metaMethod = metaMethod;
        method.paramCount = metaMethod.parameterCount();
        for (int i = 0; i < metaMethod.parameterCount(); ++i) {
          method.paramConverters.push_back(service->paramConverter(metaMethod, i));
        }
        method.returnType = metaMethod.returnType();
        method.resultConverter = service->resultConverter(method.returnType);

        methodList[metaMethod.parameterCount()] = method;
      }

      auto endpoint = [service, crossbarMethodName, methodList](const QVariantList &args, const QVariantMap &kwargs) -> QVariant {
        Q_UNUSED(kwargs);
        auto methodIterator = methodList.find(args.count());
        if (methodIterator == methodList.end()) {
          throw std::runtime_error("bad argument count");
        }

        const Method &method = *methodIterator;

        const QMetaMethod &metaMethod = method.metaMethod;
        int methodOffset = method.methodOffset;
        const QList<VoidParamConverter> &paramConverters = method.paramConverters;
        int returnType = method.returnType;
        VoidResultConverter resultConverter = method.resultConverter;

        void *result = (returnType == QMetaType::Void) ? 0 : QMetaType::create(returnType);
        void *param[11] = { result, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

        Cleaner cleaner([&result, &param, &returnType, &metaMethod]() {
          if (result) {
            QMetaType::destroy(returnType, result);
          }
          for (int i = 0; i < metaMethod.parameterCount(); ++i) {
            if (param[i + 1]) {
              QMetaType::destroy(metaMethod.parameterType(i), param[i + 1]);
            }
          }
        });

        for (int i = 0; i < args.count(); ++i) {
          try {
            param[i + 1] = convertParameter(args[i], metaMethod.parameterType(i), paramConverters[i]);
          }
          catch(const no_converter_error &) {
            throw std::runtime_error(QString(metaMethod.name() + ": no converter for parameter " + metaMethod.parameterNames()[i] + " to " + metaMethod.parameterTypes()[i]).toStdString());
          }
        }

        if (!service->qt_metacall(QMetaObject::InvokeMetaMethod, methodOffset, param)) {
          throw std::runtime_error(QString("error invoking method" + crossbarMethodName).toStdString());
        }
        return convertResult(result, returnType, resultConverter);
      };

      QWamp::Endpoint::Function wrappedEndpoint = endpoint;
      for (QWamp::EndpointWrapper wrapper : service->wrappers) {
        QWamp::Endpoint::Function innerEndpoint = wrappedEndpoint;
        wrappedEndpoint = [wrapper, innerEndpoint] (const QVariantList &args, const QVariantMap &kwargs) {
          return wrapper(args, kwargs, innerEndpoint);
        };
      }
      if (commonWrapper) {
        QWamp::Endpoint::Function innerEndpoint = wrappedEndpoint;
        wrappedEndpoint = [innerEndpoint] (const QVariantList &args, const QVariantMap &kwargs) {
          return commonWrapper(args, kwargs, innerEndpoint);
        };
      }
      session.provide(crossbarMethodName, wrappedEndpoint, service->callType);
//      if (staticWrapper) {
//        session.provide(crossbarMethodName, [endpoint] (const QVariantList &args, const QVariantMap &kwargs) {
//          return staticWrapper(args, kwargs, endpoint);
//        }, service->callType);
//      }
//      else {
//        session.provide(crossbarMethodName, endpoint, service->callType);
//      }
    }

//    for (int methodOffset = metaObject->methodOffset(); methodOffset < metaObject->methodCount(); ++methodOffset) {
//      QMetaMethod metaMethod = metaObject->method(methodOffset);
//      if (metaMethod.access() != QMetaMethod::Public) {
//        continue;
//      }
//      qDebug() << methodOffset << metaMethod.methodSignature();
//      //pokud ma fce implicitni parametry, tak se ve vyctu objevuje vicekrat, je potreba je nejak posbirat
//      //dohromady a udelat k tomu nejaky univerzalni handler
//      QStringList nameParts;
//      if (!m_prefix.isEmpty()) {
//        nameParts << m_prefix;
//      }
//      if (m_addClassName) {
//        if (service->apiClassName.isEmpty()) {
//          nameParts << metaObject->className();
//        }
//        else {
//          nameParts << service->apiClassName;
//        }
//      }
//      nameParts << metaMethod.name();
//      QString crossbarMethodName = nameParts.join(".");
//      qDebug() << "Registering" << crossbarMethodName;

//      QList<VoidParamConverter> paramConverters;
//      for (int i = 0; i < metaMethod.parameterCount(); ++i) {
//        paramConverters.push_back(service->paramConverter(metaMethod, i));
//      }
//      int returnType = metaMethod.returnType();
//      VoidResultConverter resultConverter = service->resultConverter(returnType);

//      auto endpoint = [service, metaMethod, methodOffset, crossbarMethodName, paramConverters, returnType, resultConverter](const QVariantList &args, const QVariantMap &kwargs) -> QVariant {
//        Q_UNUSED(kwargs);
//        qDebug() << "Called" << crossbarMethodName << "with:" << args;
//        if (metaMethod.parameterCount() != args.count()) {
//          throw std::runtime_error("bad argument count");
//        }

//        void *result = (returnType == QMetaType::Void) ? 0 : QMetaType::create(returnType);
//        void *param[11] = { result, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

//        Cleaner cleaner([&result, &param, &returnType, &metaMethod]() {
//          if (result) {
//            QMetaType::destroy(returnType, result);
//          }
//          for (int i = 0; i < metaMethod.parameterCount(); ++i) {
//            if (param[i + 1]) {
//              QMetaType::destroy(metaMethod.parameterType(i), param[i + 1]);
//            }
//          }
//        });

//        for (int i = 0; i < args.count(); ++i) {
//          try {
//            param[i + 1] = convertParameter(args[i], metaMethod.parameterType(i), paramConverters[i]);
//          }
//          catch(const no_converter_error &) {
//            throw std::runtime_error(QString(metaMethod.name() + ": no converter for parameter " + metaMethod.parameterNames()[i] + " to " + metaMethod.parameterTypes()[i]).toStdString());
//          }
//        }

//        if (!service->qt_metacall(QMetaObject::InvokeMetaMethod, methodOffset, param)) {
//          throw std::runtime_error(QString("error invoking method" + crossbarMethodName).toStdString());
//        }
//        return convertResult(result, returnType, resultConverter);
//      };

//      if (wrapper) {
//        session.provide(crossbarMethodName, [endpoint] (const QVariantList &args, const QVariantMap &kwargs) {
//          return wrapper(args, kwargs, endpoint);
//        });
//      }
//      else {
//        session.provide(crossbarMethodName, endpoint);
//      }
//    }
  }
}

