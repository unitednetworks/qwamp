
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
CrossbarService::EndpointWrapper CrossbarService::wrapper;

CrossbarService::CrossbarService() {
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

void CrossbarService::registerParamConverter(int type, CrossbarService::ParamConverter converter) {
  paramConverters[type] = converter;
}

void CrossbarService::registerBasicParamConverters() {
  registerSimpleParamConverter(QMetaType::QString, &QVariant::toString);
  registerSimpleParamConverter(QMetaType::Bool, &QVariant::toBool);
  registerSimpleParamConverter(QMetaType::Int, [](const QVariant &v) { return v.toInt(); });
  registerParamConverter(QMetaType::QTime, qTimeConverter);
}

void *CrossbarService::qTimeConverter(const QVariant &v) {
  if (v.canConvert(QMetaType::QString)) {
    QTime t = QTime::fromString(v.toString(), "HH:mm");
    if (t.isValid()) {
      return new QTime(t);
    }
  }
  return 0;
}

void CrossbarService::registerServices(Autobahn::Session &session) {
  if (!services) {
    return;
  }
  for (CrossbarService *service : *services) {
    const QMetaObject *metaObject = service->metaObject();
    for(int methodOffset = metaObject->methodOffset(); methodOffset < metaObject->methodCount(); ++methodOffset) {
      QMetaMethod metaMethod = metaObject->method(methodOffset);
      if (metaMethod.access() != QMetaMethod::Public) {
        continue;
      }
      //qDebug() << methodOffset << metaMethod.methodSignature();
      //pokud ma fce implicitni parametry, tak se ve vyctu objevuje vicekrat, je potreba je nejak posbirat
      //dohromady a udelat k tomu nejaky univerzalni handler
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
      nameParts << metaMethod.name();
      QString crossbarMethodName = nameParts.join(".");
      qDebug() << "Registering" << crossbarMethodName;

      auto endpoint = [service, metaMethod, methodOffset, crossbarMethodName](const QVariantList &args, const QVariantMap &kwargs)->QVariant {
        Q_UNUSED(kwargs);
        qDebug() << "Called" << crossbarMethodName << "with:" << args;
        if (metaMethod.parameterCount() != args.count()) {
          throw std::runtime_error("bad argument count");
        }
        int returnType = metaMethod.returnType();
//        const QMetaObject *returnMetaObject = QMetaType::metaObjectForType(returnType);
        void *result = 0;
        if (returnType != QMetaType::Void) {
          result = QMetaType::create(returnType);
        }

//        if (!QVariant::canConvert(metaMethod.returnType())) {
//          qDebug() << "Cannot register method" << crossbarMethodName << "because return type cannot be converted to QVariant";
//          continue;
//        }

        void *param[11];
        param[0] = result;
        for (int i = 1; i < 11; ++i) {
          param[i] = 0;
        }

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
          if (args[i].type() == QVariant::String && QMetaType::typeFlags(metaMethod.parameterType(i)) & QMetaType::IsEnumeration && !args[i].toString().isEmpty()) {
            const QMetaObject *mo = QMetaType::metaObjectForType(metaMethod.parameterType(i));
            QString pt = metaMethod.parameterTypes()[i];
            pt = pt.mid(pt.lastIndexOf(':') + 1);
            int ei = mo->indexOfEnumerator(pt.toUtf8().constData());
            if (ei == -1) {
              throw std::runtime_error(QString("can't find enum " + pt).toStdString());
            }
            int enumValue = mo->enumerator(ei).keyToValue(args[i].toString().toUtf8().constData());
            param[i + 1] = new int(enumValue);
          }
          else {
            if (args[i].isValid() && !service->paramConverters.contains(metaMethod.parameterType(i))) {
              throw std::runtime_error(QString(metaMethod.name() + ": no converter for parameter " + metaMethod.parameterNames()[i] + " to " + metaMethod.parameterTypes()[i]).toStdString());
            }
            if (args[i].isValid()) {
              param[i + 1] = service->paramConverters[metaMethod.parameterType(i)](args[i]);
              if (!param[i + 1]) {
                throw std::runtime_error(QString("can't convert input parameter " + metaMethod.parameterNames()[i] + " to " + metaMethod.parameterTypes()[i]).toStdString());
              }
            }
            else {
              param[i + 1] = QMetaType::create(metaMethod.parameterType(i));
            }
            if (args[i].isValid() && !args[i].canConvert(metaMethod.parameterType(i))) {
              throw std::runtime_error(QString("can't convert input parameter " + metaMethod.parameterNames()[i] + " to " + metaMethod.parameterTypes()[i]).toStdString());
            }
          }
//          if (args[i].isValid()) {
//            if (metaMethod.parameterType(i) == QMetaType::QTime) {
//              QTime *t = (QTime *)param[i + 1];
//              *t = QTime::fromString(args[i].toString(), "HH:mm");
//              if (!t->isValid()) {
//                throw std::runtime_error(QString("can't convert input parameter " + metaMethod.parameterNames()[i] + " to " + metaMethod.parameterTypes()[i]).toStdString());
//              }
//            }
//            else {
//              args[i].convert(metaMethod.parameterType(i), param[i + 1]);
//              if (metaMethod.parameterType(i) == QMetaType::QString) {
//                QString *s = (QString *)param[i + 1];
//                *s = args[i].toString();
//                qDebug() << args[i] << args[i].toString() << "param" << i << (*s);
//              }
//            }
//          }
        }
        if (!service->qt_metacall(QMetaObject::InvokeMetaMethod, methodOffset, param)) {
          throw std::runtime_error(QString("error invoking method" + crossbarMethodName).toStdString());
        }
        QVariant res;
        if (returnType == QMetaType::QDateTime) {
          QDateTime *d = (QDateTime *)result;
          res = QVariant(d->toString(Qt::DateFormat::ISODate));
        }
        else if (returnType == QMetaType::QTime) {
          QTime *t = (QTime *)result;
          res = QVariant(t->toString("HH:mm"));
        }
        else if (returnType >= QMetaType::User && QMetaType::hasRegisteredConverterFunction(returnType, qMetaTypeId<QVariantMap>())) {
          QVariantMap map;
          QMetaType::convert(result, returnType, &map, qMetaTypeId<QVariantMap>());
          res = QVariant(map);
        }
        else if (result) {
          res = QVariant(returnType, result);
        }
        return res;
      };

      if (wrapper) {
        session.provide(crossbarMethodName, [endpoint] (const QVariantList &args, const QVariantMap &kwargs) {
          return wrapper(args, kwargs, endpoint);
        });
      }
      else {
        session.provide(crossbarMethodName, endpoint);
      }
    }
  }
}

