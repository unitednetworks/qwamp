///////////////////////////////////////////////////////////////////////////////
//
//  Copyright (C) 2016 United Networks SE
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
///////////////////////////////////////////////////////////////////////////////

#include "qwamp.h"

#include <exception>

#include <QCoreApplication>
#include <QtConcurrent>
#include <QDebug>
#include <QFuture>
#include <QFutureWatcher>
#include <QIODevice>
#include <QString>
#include <QVariant>

#include <msgpack.h>

#include <arpa/inet.h>

namespace QWamp {

  QString HmacSHA256(const QString &secret, const QString &key) {
    // Need to do for XOR operation. Transforms QString to unsigned char
    QByteArray k = secret.toLatin1();
    // Length of secret word
    int kLength = k.length();

    // Inner padding
    QByteArray ipad;
    // Outer padding
    QByteArray opad;

    // If secret key > 64 bytes use this to obtain sha256 key
    if (kLength > 64) {
      k = QCryptographicHash::hash(k, QCryptographicHash::Sha256);
      kLength = 20;
    }

    // Fills ipad and opad with zeros
    ipad.fill(0, 64);
    opad.fill(0, 64);

    // Copies Secret to ipad and opad
    ipad.replace(0, kLength, k);
    opad.replace(0, kLength, k);

    // XOR operation for inner and outer pad
    for (int i = 0; i < 64; i++) {
      ipad[i] = ipad[i] ^ 0x36;
      opad[i] = opad[i] ^ 0x5c;
    }

    // Stores hashed content
    QByteArray context;

    // Appends XOR:ed ipad to context
    context.append(ipad, 64);
    // Appends key to context
    context.append(key);

    //Hashes inner pad
    QByteArray sha256 = QCryptographicHash::hash(context, QCryptographicHash::Sha256);

    context.clear();
    //Appends opad to context
    context.append(opad, 64);
    //Appends hashed inner pad to context
    context.append(sha256);

    // Hashes outerpad
    sha256 = QCryptographicHash::hash(context, QCryptographicHash::Sha256);

    // String to return hashed stuff in Base64 format
    return sha256.toBase64();
  }


  Session::Session(QIODevice &in, QIODevice &out, Session::Transport transport, bool debug_calls)
    : QObject(0),
      m_debug_calls(debug_calls),
      m_stopped(false),
      m_in(in),
      m_out(out),
      mIsJoined(false),
      m_msg_read(0),
      m_session_id(0),
      m_request_id(0),
      m_goodbye_sent(false),
      mTransport(transport),
      state(Initial) {
    connect(&m_in, &QIODevice::readyRead, this, &QWamp::Session::readData);
    connect(&m_in, &QIODevice::readChannelFinished, [this]() {
      m_stopped = true;
      state = Initial;
    });
//    MsgPack::registerType(QMetaType::QDateTime, 37);
    QObject::connect(this, &Session::joined, [this]() {
      provide("api.getMethods", [this](const QVariantList &, const QVariantMap &) {
        QVariantList l;
        for (auto methodIterator = m_methods.cbegin(); methodIterator != m_methods.cend(); ++ methodIterator) {
          QVariantMap st;
          QVariantList ml;
          for (const QString &method : methodIterator.value()) {
            QVariantMap m;
            m["name"] = method;
            ml << m;
          }
          st["class"] = methodIterator.key();
          st["methods"] = ml;
          l << st;
        }
        return l;
      });
    });
  }

  Session::Session(QIODevice &inout, Session::Transport transport, bool debug_calls) : Session(inout, inout, transport, debug_calls)
  {
  }

  Session::Session(const QString &name, QIODevice &in, QIODevice &out, Session::Transport transport, bool debug_calls) : Session(in, out, transport, debug_calls)
  {
    m_name = name;
  }

  Session::Session(const QString &name, QIODevice &inout, Session::Transport transport, bool debug_calls) : Session(name, inout, inout, transport, debug_calls)
  {
  }

  const QString &Session::name() const
  {
    return m_name;
  }

  void Session::setName(const QString &name)
  {
    m_name = name;
  }

  void Session::provideStatistics() {
    provide("api.getStatistics", [this](const QVariantList &args, const QVariantMap &kwargs) {
      int totalCalls = 0;
      int totalTime = 0;
      QVariantMap stl;
      for (const QString &key : m_callStatistics.keys()) {
        const CallStatistics &c = m_callStatistics[key];
        totalCalls += c.callCount;
        totalTime += c.callCount * c.averageTime;
        stl[key] = QVariantMap{{ "calls", c.callCount }, { "averageTime", c.averageTime }};
      }
      QVariantMap st;
      st["procedures"] = stl;
      st["totalCalls"] = totalCalls;
      if (totalCalls) {
        st["totalAverage"] = totalTime / totalCalls;
      }
      else {
        st["totalAverage"] = 0;
      }
      return st;
    });
  }

  void Session::start() {
    // Send the initial handshake packet informing the server which
    // serialization format we wish to use, and our maximum message size
    //
    m_buffer_msg_len[0] = 0x7F; // magic byte
    m_buffer_msg_len[1] = 0xF2; // we are ready to receive messages up to 2**24 octets and encoded using MsgPack
    m_buffer_msg_len[2] = 0x00; // reserved
    m_buffer_msg_len[3] = 0x00; // reserved
    m_out.write(m_buffer_msg_len, sizeof(m_buffer_msg_len));
  }

  void Session::readData() {
    while(m_in.bytesAvailable()) {
      if (state == Initial) {
        get_handshake_reply();
      }
      else if (state == Started) {
        get_msg_header();
      }
      else {
        get_msg_body();
      }
      if (m_stopped) {
        break;
      }
    }
  }

  void Session::get_handshake_reply() {
    m_in.read(m_buffer_msg_len, sizeof(m_buffer_msg_len));
    if (m_buffer_msg_len[0] != 0x7F) {
      throw protocol_error("invalid magic byte in RawSocket handshake response");
    }
    if (((m_buffer_msg_len[1] & 0x0F) != 0x02)) {
      // FIXME: this isn't exactly a "protocol error" => invent new exception
      throw protocol_error("RawSocket handshake reply: server does not speak MsgPack encoding");
    }

    state = Started;

    emit started();
  }

  void Session::stop() {
    m_stopped = true;
    try {
      m_in.close();
    }
    catch (...) {
    }
    try {
      m_out.close();
    }
    catch (...) {
    }
    state = Initial;
  }

  void Session::join(const QString &realm, const QString &authid, const QStringList &authmethods) {
    // [HELLO, Realm|uri, Details|dict]

    QVariantList msg;
    msg << static_cast<int>(WampMsgCode::HELLO) << realm;

    QVariantMap roles;
    roles["caller"] = QVariantMap();
    roles["callee"] = QVariantMap();
    roles["publisher"] = QVariantMap();
    roles["subscriber"] = QVariantMap();
    QVariantMap m;
    m["roles"] = roles;
    if (!authid.isEmpty()) {
      m["authid"] = authid;
      if (!authmethods.isEmpty()) {
        m["authmethods"] = authmethods;
      }
    }
    msg << m;

    send(msg);
  }

  void Session::subscribe(const QString &topic, Handler handler) {

    if (!m_session_id) {
      throw no_session_error();
    }

    // [SUBSCRIBE, Request|id, Options|dict, Topic|uri]

    m_request_id += 1;
    subscribeRequests.insert(m_request_id, SubscribeRequest(topic, handler));

    QVariantList message;
    message << static_cast<int>(WampMsgCode::SUBSCRIBE) << m_request_id << QVariantMap() << topic;
    send(message);
  }

  void Session::authenticate(const QString &credentials)
  {
    QVariantList message;
    message << static_cast<int>(WampMsgCode::AUTHENTICATE) << credentials << QVariantMap();
    send(message);
  }


  void Session::provide(const QString& procedure, Endpoint::Function endpointFunction, Endpoint::Type endpointType, const QVariantMap &options) {

     if (!m_session_id) {
        throw no_session_error();
     }

     QStringList procedureParts = procedure.split('.');
     if (procedureParts.count() == 1) {
       m_methods[""] << procedure;
     }
     else {
       m_methods[procedureParts[0]] << procedureParts.mid(1).join('.');
     }
     m_request_id += 1;
     Endpoint endpoint;
     QString procedureName = makeName(procedure);
     if (m_debug_calls) {
       Endpoint::Function wrappedEndpointFunction = [this, procedure, endpointFunction, procedureName](const QVariantList &args, const QVariantMap &kwargs)->QVariant {
         QTime timer;
         timer.start();
         QByteArray argsJson = "(";
         if (args.count()) {
           argsJson += QJsonDocument::fromVariant(args).toJson(QJsonDocument::Compact).mid(1);
           argsJson.chop(1);
         }
         argsJson += ")";
         qDebug() << "Called" << qUtf8Printable(procedureName) << argsJson.constData();
         QVariant result = endpointFunction(args, kwargs);
         int elapsed = timer.elapsed();
         qDebug() << "execution elapsed" << elapsed << "ms";
         CallStatistics &cst = m_callStatistics[procedure];
         int tm = cst.callCount * cst.averageTime + elapsed;
         ++cst.callCount;
         cst.averageTime = tm / cst.callCount;
         return result;
       };
       endpoint = { wrappedEndpointFunction, endpointType };
     }
     else {
       endpoint = { endpointFunction, endpointType };
     }

     registerRequests.insert(m_request_id, RegisterRequest(procedureName, endpoint));

     // [REGISTER, Request|id, Options|dict, Procedure|uri]
     send(QVariantList() << static_cast<int>(WampMsgCode::REGISTER) << m_request_id << options << procedureName);
  }


  void Session::publish(const QString &topic, const QVariantList &args, const QVariantMap &kwargs) {

    if (!m_session_id) {
      throw no_session_error();
    }

    m_request_id += 1;

    if (m_debug_calls) {
      QByteArray argsJson = "(";
      if (args.count()) {
        argsJson += QJsonDocument::fromVariant(args).toJson(QJsonDocument::Compact).mid(1);
        argsJson.chop(1);
      }
      argsJson += ")";
      qDebug().noquote() << "Publishing" << makeName(topic) << argsJson.constData();
    }

    // [PUBLISH, Request|id, Options|dict, Topic|uri, Arguments|list, ArgumentsKw|dict]

    QVariantList message;
    message << static_cast<int>(WampMsgCode::PUBLISH) << m_request_id << QVariantMap() << makeName(topic);

    if (args.count()) {
      message << QVariant(args);
      if (kwargs.count()) {
        message << kwargs;
      }
    }
    send(message);
  }

  QString Session::makeName(const QString &name) const
  {
    QString n = m_name;
    if (!n.isEmpty()) {
      n += QStringLiteral(".");
    }
    return n + name;
  }

  QVariantList Session::convertParams(const QVariantList &args) {
    QVariantList r;
    for (const QVariant &v : args) {
      r << convertParam(v);
    }
    return r;
  }

  QVariant Session::convertParam(const QVariant &arg) {
    if (arg.type() == QVariant::List) {
      QVariantList r;
      for (const QVariant &v : arg.toList()) {
        r << convertParam(v);
      }
      return r;
    }
    if (arg.type() == QVariant::Map) {
      QVariantMap r;
      QVariantMap map = arg.toMap();
      for (auto mapIterator = map.cbegin(); mapIterator != map.cend(); ++mapIterator) {
        r.insert(mapIterator.key(), convertParam(mapIterator.value()));
      }
      return r;
    }
    if (arg.type() == QVariant::Date) {
      return arg.toDate().toString(Qt::DateFormat::ISODate);
    }
    if (arg.type() == QVariant::DateTime) {
      return arg.toDateTime().toString(Qt::DateFormat::ISODate);
    }
    if (arg.type() != QVariant::UserType && QMetaType(arg.type()).flags() & QMetaType::IsEnumeration) {
      return arg.toString();
    }
    if (arg.type() == QVariant::UserType && QMetaType(arg.userType()).flags() & QMetaType::IsEnumeration) {
      return arg.toString();
    }
    return arg;
  }

  QVariant Session::call(const QString &procedure, const QVariantList &args, const QVariantMap &kwargs) {

    if (!m_session_id) {
      throw no_session_error();
    }

    m_request_id += 1;
    CallRequests::iterator callRequestsIterator = callRequests.insert(m_request_id, CallRequest());
    // [CALL, Request|id, Options|dict, Procedure|uri]

    QVariantList message;
    message << static_cast<int> (WampMsgCode::CALL) << m_request_id << QVariantMap() << procedure;
    if (args.count()) {
      message << QVariant(convertParams(args));
      if (kwargs.count()) {
        message << kwargs;
      }
    }
    send(message);

    CallRequest &callRequest = callRequestsIterator.value();
    while (true){
      if (!m_in.waitForReadyRead(30000)) {  //TODO handle timout as exception
        QVariantMap resultMap;
        QVariantMap exception;
        exception["what"] = "error ocured during waiting for call response";
        resultMap["exception"] = exception;
        return resultMap;
      }
      readData();

//      QCoreApplication::processEvents(); //to call readData by signal
      if (callRequest.ready) {           //add exception handling
        QVariant result;
        if (!callRequest.ex.isEmpty()) {
          QVariantMap resultMap;
          QVariantMap exception;
          exception["what"] = callRequest.ex;
          resultMap["exception"] = exception;
          result = resultMap;
        }
        else {
          result = callRequest.result;
        }
        callRequests.erase(callRequestsIterator);
        return result;
      }
    }
  }


//  void Session::packQVariant(const QVariant& value) {

//    if (value.isNull()) {
//      m_packer.pack_nil();
//    }
//    else if (value.type() == QVariant::List || value.type() == QVariant::StringList) {
//      packQVariant(value.toList());
//    }
//    else if (value.type() == QVariant::Map) {
//      packQVariant(value.toMap());
//    }
//    else if (value.type() == QVariant::Int) {
//      int val = value.toInt();
//      m_packer.pack(val);
//    }
//    else if (value.type() == QVariant::LongLong) {
//      uint64_t val = value.toLongLong();
//      m_packer.pack(val);
//    }
//    else if (value.type() == QVariant::Bool) {
//      bool val = value.toBool();
//      m_packer.pack(val);
//    }
//    else if (value.type() == QVariant::Double) {
//      double val = value.toDouble();
//      m_packer.pack(val);
//    }
//    else if (value.type() == QVariant::String) {
//      std::string val = value.toString().toStdString();
//      m_packer.pack(val);
//    }
//    else if (value.type() == QVariant::ByteArray) {
//      std::string val = value.toByteArray().toStdString();
//      m_packer.pack(val);
//    }
//    else if (value.type() == QVariant::Date) {
//      std::string val = value.toDate().toString(Qt::DateFormat::ISODate).toStdString();
//      m_packer.pack(val);
//    }
//    else if (value.type() == QVariant::DateTime) {
//      std::string val = value.toDateTime().toString(Qt::DateFormat::ISODate).toStdString();
//      m_packer.pack(val);
//    }
//    else if (value.type() != QVariant::UserType && QMetaType(value.type()).flags() & QMetaType::IsEnumeration) {
//      std::string val = value.toString().toStdString();
//      m_packer.pack(val);
//    }
//    else if (value.type() == QVariant::UserType && QMetaType(value.userType()).flags() & QMetaType::IsEnumeration) {
//      std::string val = value.toString().toStdString();
//      m_packer.pack(val);
//    }
//    else {
//      qDebug() << "Warning: don't know how to pack type" << value.typeName() << value.type();
//    }
//  }


  void Session::process_welcome(const QVariantList &msg) {
    if (msg.length() < 2) {
      throw protocol_error("Bad welcome response");
    }
    m_session_id = msg[1].toULongLong();
    mIsJoined = true;
    emit joined(m_session_id);
  }

  void Session::process_challenge(const QVariantList &msg)
  {
    if (msg.length() != 3) {
      throw protocol_error("Bad challenge response");
    }

    QString method = msg[1].toString();
    QString challengeString = msg[2].toMap()["challenge"].toString();
    QVariantMap challengeMap = QJsonDocument::fromJson(challengeString.toUtf8()).toVariant().toMap();
    Challenge challengeStruct;
    challengeStruct.authid = challengeMap["authid"].toString();
    challengeStruct.authmethod = challengeMap["authmethod"].toString();
    challengeStruct.authprovider = challengeMap["authprovider"].toString();
    challengeStruct.authrole = challengeMap["authrole"].toString();
    challengeStruct.nonce = challengeMap["nonce"].toString();
    challengeStruct.session = challengeMap["session"].toLongLong();
    challengeStruct.timestamp = QDateTime::fromString(challengeMap["timestamp"].toString(), Qt::DateFormat::ISODate);

    emit challenge(method, challengeString, challengeStruct);
  }


  void Session::process_goodbye(const QVariantList &msg) {

    m_session_id = 0;

    if (!m_goodbye_sent) {
      // if we did not initiate closing, reply ..
      // [GOODBYE, Details|dict, Reason|uri]
      send(QVariantList() << static_cast<int> (WampMsgCode::GOODBYE) << QVariantMap() << QString("wamp.error.goodbye_and_out"));
    }
    else {
      // we previously initiated closing, so this
      // is the peer reply
    }
    if (msg.length() < 3) {
      throw protocol_error("Bad goobay response");
    }
    QString reason = msg[2].toString();
    emit left(reason);
    m_goodbye_sent = false;
    mIsJoined = false;
 }


  void Session::leave(const QString &reason) {

    if (!m_session_id) {
      throw no_session_error();
    }

    m_goodbye_sent = true;
    m_session_id = 0;

    // [GOODBYE, Details|dict, Reason|uri]
    send(QVariantList() << static_cast<int>(WampMsgCode::GOODBYE) << QVariantMap() << reason);
  }

//  QVariantList Session::unpackMsg(std::vector<msgpack::object> &v) {
//    QList<QVariant> list;
//    list.reserve(v.size());
//    QTime timer;
//    timer.start();
//    qDebug() << "unpacking vector";
//    for (msgpack::object &o : v) {
//      list.append(unpackMsg(o));
//    }
//    qDebug() << "unpacked vector" << timer.elapsed();
//    return list;
//  }

//  QVariant Session::unpackMsg(msgpack::object& obj) {
//    switch (obj.type) {

//      case msgpack::type::STR:
//        return QVariant(QString::fromStdString(obj.as<std::string>()));

//      case msgpack::type::POSITIVE_INTEGER:
//        return QVariant((quint64)obj.as<uint64_t>());

//      case msgpack::type::NEGATIVE_INTEGER:
//        return QVariant((quint64)obj.as<int64_t>());

//      case msgpack::type::BOOLEAN:
//        return QVariant(obj.as<bool>());

//      case msgpack::type::FLOAT:
//        return QVariant(obj.as<double>());

//      case msgpack::type::NIL:
//        return QVariant();

//      case msgpack::type::ARRAY:
//        {
//          std::vector<msgpack::object> in_vec;
//          obj.convert(&in_vec);

//          return QVariant(unpackMsg(in_vec));
//        }

//      case msgpack::type::MAP:
//        {
//          std::map<std::string, msgpack::object> in_map;
//          obj.convert(&in_map);

//          return QVariant(unpackMsg(in_map));
//        }

//      default:
//        return QVariant();
//    }
//  }


  void Session::process_error(const QVariantList &msg) {

    // [ERROR, REQUEST.Type|int, REQUEST.Request|id, Details|dict, Error|uri]
    // [ERROR, REQUEST.Type|int, REQUEST.Request|id, Details|dict, Error|uri, Arguments|list]
    // [ERROR, REQUEST.Type|int, REQUEST.Request|id, Details|dict, Error|uri, Arguments|list, ArgumentsKw|dict]

    // message length
    //
    if (msg.length() != 5 && msg.length() != 6 && msg.length() != 7) {
      throw protocol_error("invalid ERROR message structure - length must be 5, 6 or 7");
    }

    // REQUEST.Type|int
    //
    if (!isUint64(msg[1])) {
      throw protocol_error("invalid ERROR message structure - REQUEST.Type must be an integer");
    }
    WampMsgCode request_type = static_cast<WampMsgCode>(msg[1].toInt());

    if (request_type != WampMsgCode::CALL &&
        request_type != WampMsgCode::REGISTER &&
        request_type != WampMsgCode::UNREGISTER &&
        request_type != WampMsgCode::PUBLISH &&
        request_type != WampMsgCode::SUBSCRIBE &&
        request_type != WampMsgCode::UNSUBSCRIBE) {
      throw protocol_error("invalid ERROR message - ERROR.Type must one of CALL, REGISTER, UNREGISTER, SUBSCRIBE, UNSUBSCRIBE");
    }

    // REQUEST.Request|id
    //
    if (!isUint64(msg[2])) {
      throw protocol_error("invalid ERROR message structure - REQUEST.Request must be an integer");
    }
    quint64 request_id = msg[2].toInt();

    // Details
    //
    if (msg[3].type() != QVariant::Map) {
      throw protocol_error("invalid ERROR message structure - Details must be a dictionary");
    }

    // Error|uri
    //
    if (msg[4].type() != QVariant::String) {
      throw protocol_error("invalid ERROR message - Error must be a string (URI)");
    }
    QString error = msg[4].toString();

    // Arguments|list
    //
    if (msg.length() > 5) {
      if (msg[5].type() != QVariant::List) {
        throw protocol_error("invalid ERROR message structure - Arguments must be a list");
      }
    }

    // ArgumentsKw|list
    //
    if (msg.length() > 6) {
      if (msg[6].type() != QVariant::Map) {
        throw protocol_error("invalid ERROR message structure - ArgumentsKw must be a dictionary");
      }
    }

    switch (request_type) {

      case WampMsgCode::CALL:
        {
          //
          // process CALL ERROR
          //
          CallRequests::iterator callIterator = callRequests.find(request_id);

          if (callIterator != callRequests.end()) {

            // FIXME: forward all error info .. also not sure if this is the correct
            // way to use set_exception()
            callIterator.value().ready = true;
            callIterator.value().ex = error;
          }
          else {
            throw protocol_error("bogus ERROR message for non-pending CALL request ID");
          }
        }
        break;

        // FIXME: handle other error messages
      default:
        qDebug() << QStringLiteral("unhandled ERROR message") << (int)request_type;
    }
  }

  void Session::process_invocation(const QVariantList &msg) {

    // [INVOCATION, Request|id, REGISTERED.Registration|id, Details|dict]
    // [INVOCATION, Request|id, REGISTERED.Registration|id, Details|dict, CALL.Arguments|list]
    // [INVOCATION, Request|id, REGISTERED.Registration|id, Details|dict, CALL.Arguments|list, CALL.ArgumentsKw|dict]

    if (msg.length() != 4 && msg.length() != 5 && msg.length() != 6) {
      throw protocol_error("invalid INVOCATION message structure - length must be 4, 5 or 6");
    }

    if (!isUint64(msg[1])) {
      throw protocol_error("invalid INVOCATION message structure - INVOCATION.Request must be an integer");
    }
    quint64 request_id = msg[1].toULongLong();

    if (!isUint64(msg[2])) {
      throw protocol_error("invalid INVOCATION message structure - INVOCATION.Registration must be an integer");
    }
    quint64 registration_id = msg[2].toULongLong();

    const QHash<quint64, Endpoint>::iterator endpointIterator = endpoints.find(registration_id);

    if (endpointIterator == endpoints.end()) {
      throw protocol_error("bogus RESULT message for non-pending request ID");
    }
    else {
      Endpoint endpoint = endpointIterator.value();

      if (msg[3].type() != QVariant::Map) {
        throw protocol_error("invalid INVOCATION message structure - Details must be a dictionary");
      }

      QVariantList args;
      QVariantMap kwargs;

      Endpoint::Function endpointFunction;
      if (endpointWrapper) {
        endpointFunction = [this, endpoint](const QVariantList &args, const QVariantMap &kwargs) {
          return endpointWrapper(args, kwargs, endpoint.function);
        };
      }
      else {
        endpointFunction = endpoint.function;
      }
      if (msg.length() > 4) {

        if (msg[4].type() != QVariant::List) {
          throw protocol_error("invalid INVOCATION message structure - INVOCATION.Arguments must be a list");
        }

        args = msg[4].toList();

        if (msg.length() > 5) {
          if (msg[5].type() != QVariant::Map) {
            throw protocol_error("invalid INVOCATION message structure - INVOCATION.ArgumentsKw must be a dictionary");
          }
          kwargs = msg[5].toMap();
        }
      }

      // [YIELD, INVOCATION.Request|id, Options|dict]
      // [YIELD, INVOCATION.Request|id, Options|dict, Arguments|list]
      // [YIELD, INVOCATION.Request|id, Options|dict, Arguments|list, ArgumentsKw|dict]
      try {
        if (endpoint.type == Endpoint::Sync) {
          QVariant res = endpointFunction(args, kwargs);
          send(QVariantList() << static_cast<int>(WampMsgCode::YIELD) << request_id << QVariantMap() << QVariant(QVariantList() << res));
        }
        else {
          QFutureWatcher<QVariant> *watcher = new QFutureWatcher<QVariant>();
          QFuture<QVariant> future = QtConcurrent::run(endpointFunction, args, kwargs);
          QObject::connect(watcher, &QFutureWatcher<QVariant>::finished, [this, request_id, watcher, future] {
            QVariant res = future.result();
            send(QVariantList() << static_cast<int>(WampMsgCode::YIELD) << request_id << QVariantMap() << QVariant(QVariantList() << res));
            watcher->deleteLater();
          });
          watcher->setFuture(future);
        }
      }

      // [ERROR, INVOCATION, INVOCATION.Request|id, Details|dict, Error|uri]
      // [ERROR, INVOCATION, INVOCATION.Request|id, Details|dict, Error|uri, Arguments|list]
      // [ERROR, INVOCATION, INVOCATION.Request|id, Details|dict, Error|uri, Arguments|list, ArgumentsKw|dict]

      // FIXME: implement Autobahn-specific exception with error URI
      catch (const std::exception& e) {
        // we can at least describe the error with e.what()
        //
        QVariantMap exception;
        exception["what"] = e.what();
        send(QVariantList()
             << static_cast<int>(WampMsgCode::ERROR)
             << static_cast<int> (WampMsgCode::INVOCATION)
             << request_id
             << QVariantMap()
             << "wamp.error.runtime_error"
             << QVariantList()
             << exception
             );
      }
      catch (...) {
        // no information available on actual error
        //
        send(QVariantList()
             << static_cast<int>(WampMsgCode::ERROR)
             << static_cast<int> (WampMsgCode::INVOCATION)
             << request_id
             << QVariantMap()
             << "wamp.error.runtime_error"
             );
      }

    }
  }

  void Session::process_call_result(const QVariantList &msg) {

    // [RESULT, CALL.Request|id, Details|dict]
    // [RESULT, CALL.Request|id, Details|dict, YIELD.Arguments|list]
    // [RESULT, CALL.Request|id, Details|dict, YIELD.Arguments|list, YIELD.ArgumentsKw|dict]

    if (msg.length() != 3 && msg.length() != 4 && msg.length() != 5) {
      throw protocol_error("invalid RESULT message structure - length must be 3, 4 or 5");
    }

    if (!isUint64(msg[1])) {
      throw protocol_error("invalid RESULT message structure - CALL.Request must be an integer");
    }

    quint64 request_id = msg[1].toULongLong();

    CallRequests::iterator callRequestIterator = callRequests.find(request_id);

    if (callRequestIterator != callRequests.end()) {

      CallRequest &callRequest = callRequestIterator.value();
      callRequest.ready = true;

      if (msg[2].type() != QVariant::Map) {
        throw protocol_error("invalid RESULT message structure - Details must be a dictionary");
      }

      if (msg.length() > 3) {

        if (msg[3].type() != QVariant::List) {
          throw protocol_error("invalid RESULT message structure - YIELD.Arguments must be a list");
        }

        QVariantList args = msg[3].toList();

        if (args.length() > 0) {
          callRequest.result = args[0];
        }
      }
    }
  }

  bool Session::isUint64(const QVariant &v) {
    return v.type() == QVariant::Int || v.type() == QVariant::LongLong || v.type() == QVariant::UInt || v.type() == QVariant::ULongLong;
  }

  void Session::process_subscribed(const QVariantList &msg) {

    // [SUBSCRIBED, SUBSCRIBE.Request|id, Subscription|id]

    if (msg.length() != 3) {
      throw protocol_error("invalid SUBSCRIBED message structure - length must be 3");
    }

    if (!isUint64(msg[1])) {
      throw protocol_error("invalid SUBSCRIBED message structure - SUBSCRIBED.Request must be an integer");
    }

    quint64 request_id = msg[1].toULongLong();

    SubscribeRequests::iterator subscribeRequestIterator = subscribeRequests.find(request_id);

    if (subscribeRequestIterator == subscribeRequests.end()) {
      throw protocol_error("bogus SUBSCRIBED message for non-pending request ID");
    }
    else {

      if (!isUint64(msg[2])) {
        throw protocol_error("invalid SUBSCRIBED message structure - SUBSCRIBED.Subscription must be an integer");
      }

      quint64 subscription_id = msg[2].toULongLong();

      SubscribeRequest &subscribeRequest = subscribeRequestIterator.value();

      handlers.insert(subscription_id, subscribeRequest.handler);
      emit subscribed(Subscription(subscription_id, subscribeRequest.topic));

      subscribeRequests.erase(subscribeRequestIterator);

    }
  }


  void Session::process_event(const QVariantList &msg) {

    // [EVENT, SUBSCRIBED.Subscription|id, PUBLISHED.Publication|id, Details|dict]
    // [EVENT, SUBSCRIBED.Subscription|id, PUBLISHED.Publication|id, Details|dict, PUBLISH.Arguments|list]
    // [EVENT, SUBSCRIBED.Subscription|id, PUBLISHED.Publication|id, Details|dict, PUBLISH.Arguments|list, PUBLISH.ArgumentsKw|dict]

    if (msg.length() != 4 && msg.length() != 5 && msg.length() != 6) {
      throw protocol_error("invalid EVENT message structure - length must be 4, 5 or 6");
    }

    if (!isUint64(msg[1])) {
      throw protocol_error("invalid EVENT message structure - SUBSCRIBED.Subscription must be an integer");
    }

    quint64 subscription_id = msg[1].toULongLong();

    Handlers::iterator handlerIterator = handlers.find(subscription_id);

    if (handlerIterator != handlers.end()) {

//      if (msg[2].type != msgpack::type::POSITIVE_INTEGER) {
//        throw protocol_error("invalid EVENT message structure - PUBLISHED.Publication|id must be an integer");
//      }

      //uint64_t publication_id = msg[2].as<uint64_t>();

      if (msg[3].type() != QVariant::Map) {
        throw protocol_error("invalid EVENT message structure - Details must be a dictionary");
      }

      QVariantList args;
      QVariantMap kwargs;

      if (msg.length() > 4) {
        if (msg[4].type() != QVariant::List) {
          throw protocol_error("invalid EVENT message structure - EVENT.Arguments must be a list");
        }
        args = msg[4].toList();

        if (msg.length() > 5) {
          if (msg[5].type() != QVariant::Map) {
            throw protocol_error("invalid EVENT message structure - EVENT.Arguments must be a list");
          }
          kwargs = msg[5].toMap();
        }
      }

      try {
        // now trigger the user supplied event handler ..
        //
        while (handlerIterator != handlers.end()) {
          handlerIterator.value()(args, kwargs);
          ++handlerIterator;
        }
      }
      catch (...) {
        qDebug() << "Warning: event handler fired exception";
      }
    }
    else {
      // silently swallow EVENT for non-existent subscription IDs.
      // We may have just unsubscribed, the this EVENT might be have
      // already been in-flight.
      qDebug() << "Skipping EVENT for non-existent subscription ID " << subscription_id;
    }
  }


   void Session::process_registered(const QVariantList &msg) {

      // [REGISTERED, REGISTER.Request|id, Registration|id]

      if (msg.length() != 3) {
         throw protocol_error("invalid REGISTERED message structure - length must be 3");
      }

      if (!isUint64(msg[1])) {
         throw protocol_error("invalid REGISTERED message structure - REGISTERED.Request must be an integer");
      }

      quint64 request_id = msg[1].toULongLong();

      RegisterRequests::iterator registerRequestIterator = registerRequests.find(request_id);

      if (registerRequestIterator == registerRequests.end()) {
        throw protocol_error("bogus REGISTERED message for non-pending request ID");
      }
      else {
        RegisterRequest &registerRequest = registerRequestIterator.value();

        if (!isUint64(msg[2])) {
          throw protocol_error("invalid REGISTERED message structure - REGISTERED.Registration must be an integer");
        }

        quint64 registration_id = msg[2].toULongLong();

        endpoints[registration_id] = registerRequest.endpoint;

        emit registered(Registration(registration_id, registerRequest.procedure));
        registerRequests.erase(registerRequestIterator);
      }
   }


  void Session::get_msg_header() {
    m_in.read(m_buffer_msg_len, sizeof(m_buffer_msg_len));

    quint32 *m_buffer_msg_len_p = (quint32*) &m_buffer_msg_len;
    m_msg_len = ntohl(*m_buffer_msg_len_p);
//    m_msg_len = ntohl(*((quint32*) &m_buffer_msg_len));

    // read actual message
    readBuffer.reserve(m_msg_len + 1);
    m_msg_read = 0;
    state = ReadingMessage;
  }


  void Session::get_msg_body() {
    char *buf = readBuffer.data();
    m_msg_read += m_in.read(&buf[m_msg_read], m_msg_len - m_msg_read);
    if (m_msg_read < m_msg_len) {
      return;
    }
    state = Started;
    readBuffer.resize(m_msg_len);

    QTime timer;
    timer.start();
    QVariant nv;
    if (mTransport == Transport::Msgpack) {
      nv = MsgPack::unpack(readBuffer);
    }
    else {
      QJsonParseError parseError;
      nv = QJsonDocument::fromJson(readBuffer, &parseError).toVariant();
      if (parseError.error != QJsonParseError::NoError) {
        throw protocol_error(parseError.errorString().toStdString());
      }
    }
//    qDebug() << QJsonDocument::fromVariant(nv).toJson();

    got_msg(nv);
//    msgpack::unpacked result;

//    while (m_unpacker.next(&result)) {

//      msgpack::object obj(result.get());

//      got_msg(obj);
//    }
  }

  void Session::got_msg(const QVariant &obj) {

    if (obj.type() != QVariant::List) {
      throw protocol_error("invalid message structure - message is not an array");
    }

    QVariantList msg = obj.toList();

    if (msg.length() < 1) {
      throw protocol_error("invalid message structure - missing message code");
    }

    if (!isUint64(msg[0])) {
      throw protocol_error("invalid message code type - not an integer");
    }

    WampMsgCode code = static_cast<WampMsgCode>(msg[0].toInt());

    switch (code) {
      case WampMsgCode::HELLO:
        throw protocol_error("received HELLO message unexpected for WAMP client roles");

      case WampMsgCode::WELCOME:
        process_welcome(msg);
        break;

      case WampMsgCode::ABORT:
        if (msg[2].toString() == "wamp.error.not_authorized" ||
            msg[2].toString() == "wamp.error.no_auth_method" ||
            msg[2].toString() == "wamp.error.authentication_failed") {
          throw authorization_error(msg[1].toMap()["message"].toString().toStdString());
        }
        qDebug() << "ABORT" << msg.mid(1);
        // FIXME
        break;

      case WampMsgCode::CHALLENGE:
        process_challenge(msg);
        break;

      case WampMsgCode::AUTHENTICATE:
        throw protocol_error("received AUTHENTICATE message unexpected for WAMP client roles");

      case WampMsgCode::GOODBYE:
        process_goodbye(msg);
        break;

      case WampMsgCode::HEARTBEAT:
        // FIXME
        break;

      case WampMsgCode::ERROR:
        process_error(msg);
        break;

      case WampMsgCode::PUBLISH:
        throw protocol_error("received PUBLISH message unexpected for WAMP client roles");

      case WampMsgCode::PUBLISHED:
        // FIXME
        break;

      case WampMsgCode::SUBSCRIBE:
        throw protocol_error("received SUBSCRIBE message unexpected for WAMP client roles");

      case WampMsgCode::SUBSCRIBED:
        process_subscribed(msg);
        break;

      case WampMsgCode::UNSUBSCRIBE:
        throw protocol_error("received UNSUBSCRIBE message unexpected for WAMP client roles");

      case WampMsgCode::UNSUBSCRIBED:
        // FIXME
        break;

      case WampMsgCode::EVENT:
        process_event(msg);
        break;

      case WampMsgCode::CALL:
        throw protocol_error("received CALL message unexpected for WAMP client roles");

      case WampMsgCode::CANCEL:
        throw protocol_error("received CANCEL message unexpected for WAMP client roles");

      case WampMsgCode::RESULT:
        process_call_result(msg);
        break;

      case WampMsgCode::REGISTER:
        throw protocol_error("received REGISTER message unexpected for WAMP client roles");

      case WampMsgCode::REGISTERED:
        process_registered(msg);
        break;

      case WampMsgCode::UNREGISTER:
        throw protocol_error("received UNREGISTER message unexpected for WAMP client roles");

      case WampMsgCode::UNREGISTERED:
        // FIXME
        break;

      case WampMsgCode::INVOCATION:
        process_invocation(msg);
        break;

      case WampMsgCode::INTERRUPT:
        throw protocol_error("received INTERRUPT message - not implemented");

      case WampMsgCode::YIELD:
        throw protocol_error("received YIELD message unexpected for WAMP client roles");
    }
  }

  void Session::send(const QVariantList &message) {
    if (!m_stopped) {
      QByteArray msg;
      if (mTransport == Transport::Msgpack) {
        msg = MsgPack::pack(message);
      }
      else {
        msg = QJsonDocument::fromVariant(message).toJson(QJsonDocument::Compact);
      }

      int writtenLength = 0;
      int writtenData = 0;

      // write message length prefix
      int len = htonl(msg.length());
      writtenLength += m_out.write((char*) &len, sizeof(len));
      // write actual serialized message
      char *b = msg.data();
      while(writtenData < msg.length()) {
        writtenData += m_out.write(&b[writtenData], msg.length() - writtenData);
      }
    }
  }
}
