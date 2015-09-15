
#include "crossbarservice.h"

#include <QDebug>
#include <QMetaMethod>
#include <QTime>
#include <QVariant>

#include <exception>

QList<CrossbarService*> *CrossbarService::services = 0;
QString CrossbarService::m_prefix;
bool CrossbarService::m_addClassName = true;
CrossbarService::EndpointWrapper CrossbarService::wrapper;

CrossbarService::CrossbarService() {
  if (!services) {
    services = new QList<CrossbarService*>;
  }
  services->append(this);
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
        for (int i = 0; i < args.count(); ++i) {
          if (args[i].isValid() && !args[i].canConvert(metaMethod.parameterType(i))) {
            throw std::runtime_error(QString("can't convert input parameter " + metaMethod.parameterNames()[i] + " to " + metaMethod.parameterTypes()[i]).toStdString());
          }
          param[i + 1] = QMetaType::create(metaMethod.parameterType(i));
          if (args[i].isValid()) {
            if (metaMethod.parameterType(i) == QMetaType::QTime) {
              QTime *t = (QTime *)param[i + 1];
              *t = QTime::fromString(args[i].toString(), "HH:mm");
              if (!t->isValid()) {
                throw std::runtime_error(QString("can't convert input parameter " + metaMethod.parameterNames()[i] + " to " + metaMethod.parameterTypes()[i]).toStdString());
              }
            }
            else {
              args[i].convert(metaMethod.parameterType(i), param[i + 1]);
            }
          }
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
        else if (result) {
          res = QVariant(returnType, result);
        }
        if (result) {
          QMetaType::destroy(returnType, result);
        }
        for (int i = 0; i < metaMethod.parameterCount(); ++i) {
          QMetaType::destroy(metaMethod.parameterType(i), param[i + 1]);
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

