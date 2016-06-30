#include "crossbarcomponent.h"

#include <QDebug>
#include <QMetaMethod>
#include <QTime>
#include <QVariant>

#include <exception>
#include <functional>

QList<CrossbarComponent*> *CrossbarComponent::services = 0;
QString CrossbarComponent::m_prefix;
bool CrossbarComponent::m_addClassName = true;
QWamp::EndpointWrapper CrossbarComponent::commonWrapper;
QMap<int, CrossbarComponent::VoidParamConverter> CrossbarComponent::staticParamConverters;
QMap<int, CrossbarComponent::VoidResultConverter> CrossbarComponent::staticResultConverters;

CrossbarComponent::CrossbarComponent(QWamp::Endpoint::Type callType) : callType(callType) {
  if (!services) {
    services = new QList<CrossbarComponent*>;
  }
  services->append(this);

  registerBasicParamConverters();
}

CrossbarComponent::~CrossbarComponent() {
  services->removeOne(this);
  if (services->count() == 0) {
    delete services;
  }
}

void CrossbarComponent::setPrefix(const QString &s) {
  m_prefix = s;
}

void CrossbarComponent::registerBasicParamConverters() {
  registerSimpleParamConverter<QString>(&QVariant::toString);
  registerSimpleParamConverter<QByteArray>(&QVariant::toByteArray);
  registerSimpleParamConverter<bool>(&QVariant::toBool);
  registerSimpleParamConverter<int>([](const QVariant &v) { return v.toInt(); });
  registerParamConverter<QTime>(qTimeParamConverter);
  registerParamConverter<QDateTime>(qDateTimeParamConverter);

  registerResultConverter<QTime>(qTimeResultConverter);
  registerResultConverter<QDateTime>(qDateTimeResultConverter);
}

void CrossbarComponent::qTimeParamConverter(QTime &time, const QVariant &v) {
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

void CrossbarComponent::qDateTimeParamConverter(QDateTime &dateTime, const QVariant &v) {
  if (v.isNull()) {
    dateTime = QDateTime();
  }
  else {
    if (v.canConvert(QMetaType::QString)) {
      dateTime = QDateTime::fromString(v.toString(), Qt::DateFormat::ISODate).toLocalTime();
      if (!dateTime.isValid()) {
        throw std::runtime_error("Invalid conversion to QDateTime");
      }
    }
    else {
      throw std::runtime_error("Bad qdatetime parameter");
    }
  }
}

void CrossbarComponent::qTimeResultConverter(QVariant &res, const QTime &time) {
  res = QVariant(time.toString("HH:mm"));
}

void CrossbarComponent::qDateTimeResultConverter(QVariant &res, const QDateTime &dateTime) {
  res = QVariant(dateTime.toString(Qt::DateFormat::ISODate));
}

void CrossbarComponent::addWrapper(QWamp::EndpointWrapper wrapper) {
  wrappers.append(wrapper);
}

CrossbarComponent::VoidParamConverter CrossbarComponent::paramConverter(const QMetaMethod &metaMethod, int i) const {
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

void *CrossbarComponent::convertParameter(const QVariant &arg, int parameterType, VoidParamConverter converter) {
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

CrossbarComponent::VoidResultConverter CrossbarComponent::resultConverter(int returnType) const {
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

QVariant CrossbarComponent::convertResult(void *result, int returnType, VoidResultConverter converter) {
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

QString CrossbarComponent::methodPublishedName(CrossbarComponent *service, const QString &methodName) {
  QStringList nameParts;
  if (!m_prefix.isEmpty()) {
    nameParts << m_prefix;
  }
  if (m_addClassName) {
    if (service->apiClassName.isEmpty()) {
      nameParts << service->metaObject()->className();
    }
    else {
      nameParts << service->apiClassName;
    }
  }
  nameParts << methodName;
  return nameParts.join(".");
}

void CrossbarComponent::registerServices(QWamp::Session &session) {
  if (!services) {
    return;
  }
  const QMetaObject &metaSignalPublisher = SignalPublisher::staticMetaObject;
  QMetaMethod metaPublish = metaSignalPublisher.method(metaSignalPublisher.indexOfSlot("publish()"));
  for (CrossbarComponent *service : *services) {
    const QMetaObject *metaObject = service->metaObject();
    QMap<QString, QList<int>> methods;
    QMap<QString, QList<int>> signales;
    for (int methodOffset = metaObject->methodOffset(); methodOffset < metaObject->methodCount(); ++methodOffset) {
      QMetaMethod metaMethod = metaObject->method(methodOffset);
      if (metaMethod.access() != QMetaMethod::Public) {
        continue;
      }
      if (metaMethod.methodType() == QMetaMethod::Slot || metaMethod.methodType() == QMetaMethod::Method) {
        methods[metaMethod.name()].append(methodOffset);
      }
      else if (metaMethod.methodType() == QMetaMethod::Signal) {
        signales[metaMethod.name()].append(methodOffset);
      }
    }
    for (auto signalIterator = signales.constBegin(); signalIterator != signales.constEnd(); ++signalIterator) {
      QString crossbarTopicName = methodPublishedName(service, signalIterator.key());
      qDebug() << "Registering" << (session.name().isEmpty() ? crossbarTopicName : session.name() + "." + crossbarTopicName) << qPrintable(signalIterator.value().count() > 1 ? ("(" + QString::number(signalIterator.value().count()) + "x)") : QString());
      for (int signalIndex : signalIterator.value()) {
        QString signalId = QString() + QString::number(QSIGNAL_CODE) + metaObject->method(signalIndex).methodSignature();
        SignalPublisher *publisher = new SignalPublisher(service, signalId, crossbarTopicName, session);
        QObject::connect(service, metaObject->method(signalIndex), publisher, metaPublish);
      }
    }
    for (auto methodIterator = methods.constBegin(); methodIterator != methods.constEnd(); ++methodIterator) {
      QString crossbarMethodName = methodPublishedName(service, methodIterator.key());
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
    }
  }
}

SignalPublisher::SignalPublisher(CrossbarComponent *service, const QString &signalName, const QString &topic, QWamp::Session &session)
  : QObject(service),
    topic(topic),
    session(session),
    signalSpy(service, qPrintable(signalName))
{
}

void SignalPublisher::publish()
{
  //still works only with basic types, mappers from CrossbarService are not supported
  QList<QVariant> arguments = signalSpy.takeFirst();
  session.publish(topic, arguments);
}