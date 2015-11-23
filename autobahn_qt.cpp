///////////////////////////////////////////////////////////////////////////////
//
//  Copyright (C) 2015 Martin Spirk
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

#include "autobahn_qt.h"

#include <exception>

#include <QCoreApplication>
#include <QtConcurrent/QtConcurrent>
#include <QDebug>
#include <QFuture>
#include <QFutureWatcher>
#include <QIODevice>
#include <QString>
#include <QVariant>

namespace Autobahn {

  Session::Session(QIODevice &in, QIODevice &out, bool debug)
    : QObject(0),
      m_debug(debug),
      m_stopped(false),
      m_in(in),
      m_out(out),
      mIsJoined(false),
      m_packer(&m_buffer),
      m_session_id(0),
      m_request_id(0),
      m_goodbye_sent(false),
      state(Initial) {
    connect(&m_in, &QIODevice::readyRead, this, &Autobahn::Session::readData);
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
      else {
        get_message();
      }
    }
  }

  void Session::get_handshake_reply() {
    m_in.read(m_buffer_msg_len, sizeof(m_buffer_msg_len));
    if (m_debug) {
      qDebug() << "RawSocket handshake reply received";
    }
    if (m_buffer_msg_len[0] != 0x7F) {
      throw protocol_error("invalid magic byte in RawSocket handshake response");
    }
    if (((m_buffer_msg_len[1] & 0x0F) != 0x02)) {
      // FIXME: this isn't exactly a "protocol error" => invent new exception
      throw protocol_error("RawSocket handshake reply: server does not speak MsgPack encoding");
    }
    if (m_debug) {
      qDebug() << "RawSocket handshake reply is valid: start WAMP message send-receive loop";
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

  void Session::join(const QString& realm) {
    // [HELLO, Realm|uri, Details|dict]

    m_packer.pack_array(3);

    m_packer.pack(static_cast<int> (msg_code::HELLO));
    m_packer.pack(realm.toStdString());

    m_packer.pack_map(1);
    m_packer.pack(std::string("roles"));

    m_packer.pack_map(4);

    m_packer.pack(std::string("caller"));
    m_packer.pack_map(0);

    m_packer.pack(std::string("callee"));
    m_packer.pack_map(0);

    m_packer.pack(std::string("publisher"));
    m_packer.pack_map(0);

    m_packer.pack(std::string("subscriber"));
    m_packer.pack_map(0);

    send();
  }


   void Session::subscribe(const QString &topic, Handler handler) {

      if (!m_session_id) {
         throw no_session_error();
      }

      // [SUBSCRIBE, Request|id, Options|dict, Topic|uri]

      m_request_id += 1;
      subscribeRequests.insert(m_request_id, SubscribeRequest(topic, handler));

      m_packer.pack_array(4);
      m_packer.pack(static_cast<int> (msg_code::SUBSCRIBE));
      m_packer.pack(m_request_id);
      m_packer.pack_map(0);
      m_packer.pack(topic.toStdString());
      send();
   }


  void Session::provide(const QString& procedure, Endpoint::Function endpointFunction, Endpoint::Type endpointType, const QVariantMap &options) {

     if (!m_session_id) {
        throw no_session_error();
     }

     m_request_id += 1;
     Endpoint endpoint({ endpointFunction, endpointType });
     registerRequests.insert(m_request_id, RegisterRequest(procedure, endpoint));

     // [REGISTER, Request|id, Options|dict, Procedure|uri]

     m_packer.pack_array(4);
     m_packer.pack(static_cast<int> (msg_code::REGISTER));
     m_packer.pack(m_request_id);
     //m_packer.pack_map(0);
     packQVariant(options);
     m_packer.pack(procedure.toStdString());
     send();
  }


  void Session::publish(const QString& topic) {

    if (!m_session_id) {
      throw no_session_error();
    }

    m_request_id += 1;

    // [PUBLISH, Request|id, Options|dict, Topic|uri]

    m_packer.pack_array(4);
    m_packer.pack(static_cast<int> (msg_code::PUBLISH));
    m_packer.pack(m_request_id);
    m_packer.pack_map(0);
    m_packer.pack(topic.toStdString());
    send();
  }


  void Session::publish(const QString& topic, const QVariantList& args) {

    if (!m_session_id) {
      throw no_session_error();
    }

    if (args.count() > 0) {
      m_request_id += 1;

      // [PUBLISH, Request|id, Options|dict, Topic|uri, Arguments|list]

      m_packer.pack_array(5);
      m_packer.pack(static_cast<int> (msg_code::PUBLISH));
      m_packer.pack(m_request_id);
      m_packer.pack_map(0);
      m_packer.pack(topic.toStdString());
      packQVariant(args);
      send();
    }
    else {
      publish(topic);
    }
  }


  void Session::publish(const QString& topic, const QVariantList& args, const QVariantMap& kwargs) {

    if (!m_session_id) {
      throw no_session_error();
    }

    if (kwargs.count() > 0) {
      m_request_id += 1;

      // [PUBLISH, Request|id, Options|dict, Topic|uri, Arguments|list, ArgumentsKw|dict]

      m_packer.pack_array(6);
      m_packer.pack(static_cast<int> (msg_code::PUBLISH));
      m_packer.pack(m_request_id);
      m_packer.pack_map(0);
      m_packer.pack(topic.toStdString());
      packQVariant(args);
      packQVariant(kwargs);
      send();
    }
    else {
      publish(topic, args);
    }
  }

  QVariant Session::makeCall(const QString& procedure, std::function<void()> paramCallback) {

    if (!m_session_id) {
      throw no_session_error();
    }

    m_request_id += 1;
    CallRequests::iterator callRequestsIterator = callRequests.insert(m_request_id, CallRequest());

    // [CALL, Request|id, Options|dict, Procedure|uri]

    m_packer.pack_array(4);
    m_packer.pack(static_cast<int> (msg_code::CALL));
    m_packer.pack(m_request_id);
    m_packer.pack_map(0);
    m_packer.pack(procedure.toStdString());
    if (paramCallback) {
      paramCallback();
    }
    send();

    CallRequest &callRequest = callRequestsIterator.value();
    while (true){
      m_in.waitForReadyRead(30000);  //TODO handle timout as exception
      QCoreApplication::processEvents(); //to call readData by signal
      if (callRequest.ready) {           //add exception handling
        QVariant result = callRequest.result;
        callRequests.erase(callRequestsIterator);
        return result;
      }
    }
  }

  QVariant Session::call(const QString& procedure) {
    return makeCall(procedure);
  }


  QVariant Session::call(const QString& procedure, const QVariantList& args) {

    if (args.count() > 0) {
      return makeCall(procedure, [&]() {
        packQVariant(args);
      });
    }
    else {
      return call(procedure);
    }
  }


  QVariant Session::call(const QString& procedure, const QVariantList& args, const QVariantMap& kwargs) {

    if (kwargs.count() > 0) {
      return makeCall(procedure, [&]() {
        packQVariant(args);
        packQVariant(kwargs);
      });
    }
    else {
      return call(procedure, args);
    }
  }


  void Session::packQVariant(const QVariantList& list) {
    m_packer.pack_array(list.count());

    for (const QVariant &v : list) {
      packQVariant(v);
    }
  }

  void Session::packQVariant(const QVariantMap& map) {
    m_packer.pack_map(map.count());

    QMap<QString, QVariant>::const_iterator i = map.constBegin();
    while (i != map.constEnd()) {
      m_packer.pack(i.key().toStdString());
      packQVariant(i.value());
      ++i;
    }
  }

  void Session::packQVariant(const QVariant& value) {

    if (value.isNull()) {
      m_packer.pack_nil();
    }
    else if (value.type() == QVariant::List || value.type() == QVariant::StringList) {
      packQVariant(value.toList());
    }
    else if (value.type() == QVariant::Map) {
      packQVariant(value.toMap());
    }
    else if (value.type() == QVariant::Int) {
      int val = value.toInt();
      m_packer.pack(val);
    }
    else if (value.type() == QVariant::LongLong) {
      uint64_t val = value.toLongLong();
      m_packer.pack(val);
    }
    else if (value.type() == QVariant::Bool) {
      bool val = value.toBool();
      m_packer.pack(val);
    }
    else if (value.type() == QVariant::Double) {
      double val = value.toDouble();
      m_packer.pack(val);
    }
    else if (value.type() == QVariant::String) {
      std::string val = value.toString().toStdString();
      m_packer.pack(val);
    }
    else if (value.type() == QVariant::ByteArray) {
      std::string val = value.toByteArray().toStdString();
      m_packer.pack(val);
    }
    else if (value.type() == QVariant::Date) {
      std::string val = value.toDate().toString(Qt::DateFormat::ISODate).toStdString();
      m_packer.pack(val);
    }
    else if (value.type() == QVariant::DateTime) {
      std::string val = value.toDateTime().toString(Qt::DateFormat::ISODate).toStdString();
      m_packer.pack(val);
    }
    else if (value.type() != QVariant::UserType && QMetaType(value.type()).flags() & QMetaType::IsEnumeration) {
      std::string val = value.toString().toStdString();
      m_packer.pack(val);
    }
    else if (value.type() == QVariant::UserType && QMetaType(value.userType()).flags() & QMetaType::IsEnumeration) {
      std::string val = value.toString().toStdString();
      m_packer.pack(val);
    }
    else {
      qDebug() << "Warning: don't know how to pack type" << value.typeName();
    }
  }


  void Session::process_welcome(const wamp_msg_t& msg) {
    m_session_id = msg[1].as<uint64_t>();
    mIsJoined = true;
    emit joined(m_session_id);
  }


  void Session::process_goodbye(const wamp_msg_t& msg) {

    m_session_id = 0;

    if (!m_goodbye_sent) {

      // if we did not initiate closing, reply ..

      // [GOODBYE, Details|dict, Reason|uri]

      m_packer.pack_array(3);

      m_packer.pack(static_cast<int> (msg_code::GOODBYE));
      m_packer.pack_map(0);
      m_packer.pack(std::string("wamp.error.goodbye_and_out"));
      send();
    }
    else {
      // we previously initiated closing, so this
      // is the peer reply
    }
    std::string reason = msg[2].as<std::string>();
    emit left(QString::fromStdString(reason));
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

    m_packer.pack_array(3);

    m_packer.pack(static_cast<int> (msg_code::GOODBYE));
    m_packer.pack_map(0);
    m_packer.pack(reason.toStdString());
    send();
  }

  QVariantList Session::unpackMsg(std::vector<msgpack::object> &v) {
    QList<QVariant> list;
    for (msgpack::object &o : v) {
      list.append(unpackMsg(o));
    }
    return list;
  }

  QVariantMap Session::unpackMsg(std::map<std::string, msgpack::object> &m) {
    QVariantMap map;
    for (auto &in : m) {
      map[QString::fromStdString(in.first)] = unpackMsg(in.second);
    }
    return map;
  }

  QVariant Session::unpackMsg(msgpack::object& obj) {
    switch (obj.type) {

      case msgpack::type::STR:
        return QVariant(QString::fromStdString(obj.as<std::string>()));

      case msgpack::type::POSITIVE_INTEGER:
        return QVariant((qulonglong)obj.as<uint64_t>());

      case msgpack::type::NEGATIVE_INTEGER:
        return QVariant((qulonglong)obj.as<int64_t>());

      case msgpack::type::BOOLEAN:
        return QVariant(obj.as<bool>());

      case msgpack::type::FLOAT:
        return QVariant(obj.as<double>());

      case msgpack::type::NIL:
        return QVariant();

      case msgpack::type::ARRAY:
        {
          std::vector<msgpack::object> in_vec;
          obj.convert(&in_vec);

          return QVariant(unpackMsg(in_vec));
        }

      case msgpack::type::MAP:
        {
          std::map<std::string, msgpack::object> in_map;
          obj.convert(&in_map);

          return QVariant(unpackMsg(in_map));
        }

      default:
        return QVariant();
    }
  }


  void Session::process_error(const wamp_msg_t& msg) {

    // [ERROR, REQUEST.Type|int, REQUEST.Request|id, Details|dict, Error|uri]
    // [ERROR, REQUEST.Type|int, REQUEST.Request|id, Details|dict, Error|uri, Arguments|list]
    // [ERROR, REQUEST.Type|int, REQUEST.Request|id, Details|dict, Error|uri, Arguments|list, ArgumentsKw|dict]

    // message length
    //
    if (msg.size() != 5 && msg.size() != 6 && msg.size() != 7) {
      throw protocol_error("invalid ERROR message structure - length must be 5, 6 or 7");
    }

    // REQUEST.Type|int
    //
    if (msg[1].type != msgpack::type::POSITIVE_INTEGER) {
      throw protocol_error("invalid ERROR message structure - REQUEST.Type must be an integer");
    }
    msg_code request_type = static_cast<msg_code> (msg[1].as<int>());

    if (request_type != msg_code::CALL &&
        request_type != msg_code::REGISTER &&
        request_type != msg_code::UNREGISTER &&
        request_type != msg_code::PUBLISH &&
        request_type != msg_code::SUBSCRIBE &&
        request_type != msg_code::UNSUBSCRIBE) {
      throw protocol_error("invalid ERROR message - ERROR.Type must one of CALL, REGISTER, UNREGISTER, SUBSCRIBE, UNSUBSCRIBE");
    }

    // REQUEST.Request|id
    //
    if (msg[2].type != msgpack::type::POSITIVE_INTEGER) {
      throw protocol_error("invalid ERROR message structure - REQUEST.Request must be an integer");
    }
    uint64_t request_id = msg[2].as<uint64_t>();

    // Details
    //
    if (msg[3].type != msgpack::type::MAP) {
      throw protocol_error("invalid ERROR message structure - Details must be a dictionary");
    }

    // Error|uri
    //
    if (msg[4].type != msgpack::type::STR) {
      throw protocol_error("invalid ERROR message - Error must be a string (URI)");
    }
    std::string error = msg[4].as<std::string>();

    // Arguments|list
    //
    if (msg.size() > 5) {
      if (msg[5].type  != msgpack::type::ARRAY) {
        throw protocol_error("invalid ERROR message structure - Arguments must be a list");
      }
    }

    // ArgumentsKw|list
    //
    if (msg.size() > 6) {
      if (msg[6].type  != msgpack::type::MAP) {
        throw protocol_error("invalid ERROR message structure - ArgumentsKw must be a dictionary");
      }
    }

    switch (request_type) {

      case msg_code::CALL:
        {
          //
          // process CALL ERROR
          //
          CallRequests::iterator callIterator = callRequests.find(request_id);

          if (callIterator != callRequests.end()) {

            // FIXME: forward all error info .. also not sure if this is the correct
            // way to use set_exception()
            callIterator.value().ready = true;
            callIterator.value().ex = std::runtime_error(error);
          }
          else {
            throw protocol_error("bogus ERROR message for non-pending CALL request ID");
          }
        }
        break;

        // FIXME: handle other error messages
      default:
        qDebug() << "unhandled ERROR message" << (int)request_type;
    }
  }

  void Session::process_invocation(const wamp_msg_t& msg) {

    // [INVOCATION, Request|id, REGISTERED.Registration|id, Details|dict]
    // [INVOCATION, Request|id, REGISTERED.Registration|id, Details|dict, CALL.Arguments|list]
    // [INVOCATION, Request|id, REGISTERED.Registration|id, Details|dict, CALL.Arguments|list, CALL.ArgumentsKw|dict]

    if (msg.size() != 4 && msg.size() != 5 && msg.size() != 6) {
      throw protocol_error("invalid INVOCATION message structure - length must be 4, 5 or 6");
    }

    if (msg[1].type != msgpack::type::POSITIVE_INTEGER) {
      throw protocol_error("invalid INVOCATION message structure - INVOCATION.Request must be an integer");
    }
    uint64_t request_id = msg[1].as<uint64_t>();

    if (msg[2].type != msgpack::type::POSITIVE_INTEGER) {
      throw protocol_error("invalid INVOCATION message structure - INVOCATION.Registration must be an integer");
    }
    uint64_t registration_id = msg[2].as<uint64_t>();

    const QMap<uint64_t, Endpoint>::iterator endpointIterator = endpoints.find(registration_id);

    if (endpointIterator == endpoints.end()) {
      throw protocol_error("bogus RESULT message for non-pending request ID");
    }
    else {
      Endpoint endpoint = endpointIterator.value();

      if (msg[3].type != msgpack::type::MAP) {
        throw protocol_error("invalid INVOCATION message structure - Details must be a dictionary");
      }

      QVariantList args;
      QVariantMap kwargs;

      if (msg.size() > 4) {

        if (msg[4].type != msgpack::type::ARRAY) {
          throw protocol_error("invalid INVOCATION message structure - INVOCATION.Arguments must be a list");
        }

        std::vector<msgpack::object> raw_args;
        msg[4].convert(&raw_args);
        args = unpackMsg(raw_args);

        if (msg.size() > 5) {
          std::map<std::string, msgpack::object> raw_kwargs;
          msg[5].convert(&raw_kwargs);
          kwargs = unpackMsg(raw_kwargs);
        }
      }

      // [YIELD, INVOCATION.Request|id, Options|dict]
      // [YIELD, INVOCATION.Request|id, Options|dict, Arguments|list]
      // [YIELD, INVOCATION.Request|id, Options|dict, Arguments|list, ArgumentsKw|dict]
      try {

        if (m_debug) {
          qDebug() << "Invoking endpoint registered under " << registration_id << " as of type Endpoint";
        }

        if (endpoint.type == Endpoint::Sync) {
          QVariant res = endpoint.function(args, kwargs);
          m_packer.pack_array(4);
          m_packer.pack(static_cast<int> (msg_code::YIELD));
          m_packer.pack(request_id);
          m_packer.pack_map(0);
          m_packer.pack_array(1);
          packQVariant(res);
          send();
        }
        else {
          QFutureWatcher<QVariant> *watcher = new QFutureWatcher<QVariant>();
          QFuture<QVariant> future = QtConcurrent::run(endpoint.function, args, kwargs);
          watcher->setFuture(future);
          QObject::connect(watcher, &QFutureWatcher<QVariant>::finished, [this, request_id, watcher, future] {
            QVariant res = future.result();
            m_packer.pack_array(4);
            m_packer.pack(static_cast<int> (msg_code::YIELD));
            m_packer.pack(request_id);
            m_packer.pack_map(0);
            m_packer.pack_array(1);
            packQVariant(res);
            send();
            watcher->deleteLater();
          });
        }
      }

      // [ERROR, INVOCATION, INVOCATION.Request|id, Details|dict, Error|uri]
      // [ERROR, INVOCATION, INVOCATION.Request|id, Details|dict, Error|uri, Arguments|list]
      // [ERROR, INVOCATION, INVOCATION.Request|id, Details|dict, Error|uri, Arguments|list, ArgumentsKw|dict]

      // FIXME: implement Autobahn-specific exception with error URI
      catch (const std::exception& e) {
        // we can at least describe the error with e.what()
        //
        m_packer.pack_array(7);
        m_packer.pack(static_cast<int> (msg_code::ERROR));
        m_packer.pack(static_cast<int> (msg_code::INVOCATION));
        m_packer.pack(request_id);
        m_packer.pack_map(0);
        m_packer.pack(std::string("wamp.error.runtime_error"));
        m_packer.pack_array(0);

        m_packer.pack_map(1);

        m_packer.pack(std::string("what"));
        m_packer.pack(std::string(e.what()));

        send();
      }
      catch (...) {
        // no information available on actual error
        //
        m_packer.pack_array(5);
        m_packer.pack(static_cast<int> (msg_code::ERROR));
        m_packer.pack(static_cast<int> (msg_code::INVOCATION));
        m_packer.pack(request_id);
        m_packer.pack_map(0);
        m_packer.pack(std::string("wamp.error.runtime_error"));
        send();
      }

    }
  }

  void Session::process_call_result(const wamp_msg_t& msg) {

    // [RESULT, CALL.Request|id, Details|dict]
    // [RESULT, CALL.Request|id, Details|dict, YIELD.Arguments|list]
    // [RESULT, CALL.Request|id, Details|dict, YIELD.Arguments|list, YIELD.ArgumentsKw|dict]

    if (msg.size() != 3 && msg.size() != 4 && msg.size() != 5) {
      throw protocol_error("invalid RESULT message structure - length must be 3, 4 or 5");
    }

    if (msg[1].type != msgpack::type::POSITIVE_INTEGER) {
      throw protocol_error("invalid RESULT message structure - CALL.Request must be an integer");
    }

    uint64_t request_id = msg[1].as<uint64_t>();

    CallRequests::iterator callRequestIterator = callRequests.find(request_id);

    if (callRequestIterator != callRequests.end()) {

      CallRequest &callRequest = callRequestIterator.value();
      callRequest.ready = true;

      if (msg[2].type != msgpack::type::MAP) {
        throw protocol_error("invalid RESULT message structure - Details must be a dictionary");
      }

      if (msg.size() > 3) {

        if (msg[3].type != msgpack::type::ARRAY) {
          throw protocol_error("invalid RESULT message structure - YIELD.Arguments must be a list");
        }

        std::vector<msgpack::object> raw_args;
        msg[3].convert(&raw_args);

        QVariantList args = unpackMsg(raw_args);

        if (args.size() > 0) {
          callRequest.result = args[0];
        }
      }
    }
  }


  void Session::process_subscribed(const wamp_msg_t& msg) {

    // [SUBSCRIBED, SUBSCRIBE.Request|id, Subscription|id]

    if (msg.size() != 3) {
      throw protocol_error("invalid SUBSCRIBED message structure - length must be 3");
    }

    if (msg[1].type != msgpack::type::POSITIVE_INTEGER) {
      throw protocol_error("invalid SUBSCRIBED message structure - SUBSCRIBED.Request must be an integer");
    }

    uint64_t request_id = msg[1].as<uint64_t>();

    SubscribeRequests::iterator subscribeRequestIterator = subscribeRequests.find(request_id);

    if (subscribeRequestIterator == subscribeRequests.end()) {
      throw protocol_error("bogus SUBSCRIBED message for non-pending request ID");
    }
    else {

      if (msg[2].type != msgpack::type::POSITIVE_INTEGER) {
        throw protocol_error("invalid SUBSCRIBED message structure - SUBSCRIBED.Subscription must be an integer");
      }

      uint64_t subscription_id = msg[2].as<uint64_t>();

      SubscribeRequest &subscribeRequest = subscribeRequestIterator.value();

      handlers.insert(subscription_id, subscribeRequest.handler);
      emit subscribed(Subscription(subscription_id, subscribeRequest.topic));

      subscribeRequests.erase(subscribeRequestIterator);

    }
  }


  void Session::process_event(const wamp_msg_t& msg) {

    // [EVENT, SUBSCRIBED.Subscription|id, PUBLISHED.Publication|id, Details|dict]
    // [EVENT, SUBSCRIBED.Subscription|id, PUBLISHED.Publication|id, Details|dict, PUBLISH.Arguments|list]
    // [EVENT, SUBSCRIBED.Subscription|id, PUBLISHED.Publication|id, Details|dict, PUBLISH.Arguments|list, PUBLISH.ArgumentsKw|dict]

    if (msg.size() != 4 && msg.size() != 5 && msg.size() != 6) {
      throw protocol_error("invalid EVENT message structure - length must be 4, 5 or 6");
    }

    if (msg[1].type != msgpack::type::POSITIVE_INTEGER) {
      throw protocol_error("invalid EVENT message structure - SUBSCRIBED.Subscription must be an integer");
    }

    uint64_t subscription_id = msg[1].as<uint64_t>();

    Handlers::iterator handlerIterator = handlers.find(subscription_id);

    if (handlerIterator != handlers.end()) {

      if (msg[2].type != msgpack::type::POSITIVE_INTEGER) {
        throw protocol_error("invalid EVENT message structure - PUBLISHED.Publication|id must be an integer");
      }

      //uint64_t publication_id = msg[2].as<uint64_t>();

      if (msg[3].type != msgpack::type::MAP) {
        throw protocol_error("invalid EVENT message structure - Details must be a dictionary");
      }

      QVariantList args;
      QVariantMap kwargs;

      if (msg.size() > 4) {

        if (msg[4].type != msgpack::type::ARRAY) {
          throw protocol_error("invalid EVENT message structure - EVENT.Arguments must be a list");
        }

        std::vector<msgpack::object> raw_args;
        msg[4].convert(&raw_args);
        args = unpackMsg(raw_args);

        if (msg.size() > 5) {

          if (msg[5].type != msgpack::type::MAP) {
            throw protocol_error("invalid EVENT message structure - EVENT.Arguments must be a list");
          }

          std::map<std::string, msgpack::object> raw_kwargs;
          msg[5].convert(&raw_kwargs);
          kwargs = unpackMsg(raw_kwargs);
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
        if (m_debug) {
          qDebug() << "Warning: event handler fired exception";
        }
      }

    }
    else {
      // silently swallow EVENT for non-existent subscription IDs.
      // We may have just unsubscribed, the this EVENT might be have
      // already been in-flight.
      if (m_debug) {
        qDebug() << "Skipping EVENT for non-existent subscription ID " << subscription_id;
      }
    }
  }


   void Session::process_registered(const wamp_msg_t& msg) {

      // [REGISTERED, REGISTER.Request|id, Registration|id]

      if (msg.size() != 3) {
         throw protocol_error("invalid REGISTERED message structure - length must be 3");
      }

      if (msg[1].type != msgpack::type::POSITIVE_INTEGER) {
         throw protocol_error("invalid REGISTERED message structure - REGISTERED.Request must be an integer");
      }

      uint64_t request_id = msg[1].as<uint64_t>();

      RegisterRequests::iterator registerRequestIterator = registerRequests.find(request_id);

      if (registerRequestIterator == registerRequests.end()) {
        throw protocol_error("bogus REGISTERED message for non-pending request ID");
      }
      else {
        RegisterRequest &registerRequest = registerRequestIterator.value();

        if (msg[2].type != msgpack::type::POSITIVE_INTEGER) {
          throw protocol_error("invalid REGISTERED message structure - REGISTERED.Registration must be an integer");
        }

        uint64_t registration_id = msg[2].as<uint64_t>();

        endpoints[registration_id] = registerRequest.endpoint;

        emit registered(Registration(request_id, registerRequest.procedure));
        registerRequests.erase(registerRequestIterator);
      }
   }


  void Session::get_message() {
    get_msg_header();
    get_msg_body();
  }

  void Session::get_msg_header() {
    m_in.read(m_buffer_msg_len, sizeof(m_buffer_msg_len));

    m_msg_len = ntohl(*((uint32_t*) &m_buffer_msg_len));

    if (m_debug) {
      qDebug() << "RX message (" << m_msg_len << " octets) ...";
    }

    // read actual message
    m_unpacker.reserve_buffer(m_msg_len);
  }


  void Session::get_msg_body() {
    char *buf = m_unpacker.buffer();
    qint64 read = 0;
    while (read < m_msg_len) {
      m_in.waitForReadyRead(100);
      read += m_in.read(&buf[read], m_msg_len - read);
    }
    if (m_debug) {
      qDebug() << "RX message received.";
    }

    m_unpacker.buffer_consumed(m_msg_len);

    msgpack::unpacked result;

    while (m_unpacker.next(&result)) {

      msgpack::object obj(result.get());

      if (m_debug) {
        qDebug() << "RX WAMP message ";
      }

      got_msg(obj);
    }
  }

  void Session::got_msg(const msgpack::object& obj) {

    if (obj.type != msgpack::type::ARRAY) {
      throw protocol_error("invalid message structure - message is not an array");
    }

    wamp_msg_t msg;
    obj.convert(&msg);

    if (msg.size() < 1) {
      throw protocol_error("invalid message structure - missing message code");
    }

    if (msg[0].type != msgpack::type::POSITIVE_INTEGER) {
      throw protocol_error("invalid message code type - not an integer");
    }

    msg_code code = static_cast<msg_code> (msg[0].as<int>());

    switch (code) {
      case msg_code::HELLO:
        throw protocol_error("received HELLO message unexpected for WAMP client roles");

      case msg_code::WELCOME:
        process_welcome(msg);
        break;

      case msg_code::ABORT:
        // FIXME
        break;

      case msg_code::CHALLENGE:
        throw protocol_error("received CHALLENGE message - not implemented");

      case msg_code::AUTHENTICATE:
        throw protocol_error("received AUTHENTICATE message unexpected for WAMP client roles");

      case msg_code::GOODBYE:
        process_goodbye(msg);
        break;

      case msg_code::HEARTBEAT:
        // FIXME
        break;

      case msg_code::ERROR:
        process_error(msg);
        break;

      case msg_code::PUBLISH:
        throw protocol_error("received PUBLISH message unexpected for WAMP client roles");

      case msg_code::PUBLISHED:
        // FIXME
        break;

      case msg_code::SUBSCRIBE:
        throw protocol_error("received SUBSCRIBE message unexpected for WAMP client roles");

      case msg_code::SUBSCRIBED:
        process_subscribed(msg);
        break;

      case msg_code::UNSUBSCRIBE:
        throw protocol_error("received UNSUBSCRIBE message unexpected for WAMP client roles");

      case msg_code::UNSUBSCRIBED:
        // FIXME
        break;

      case msg_code::EVENT:
        process_event(msg);
        break;

      case msg_code::CALL:
        throw protocol_error("received CALL message unexpected for WAMP client roles");

      case msg_code::CANCEL:
        throw protocol_error("received CANCEL message unexpected for WAMP client roles");

      case msg_code::RESULT:
        process_call_result(msg);
        break;

      case msg_code::REGISTER:
        throw protocol_error("received REGISTER message unexpected for WAMP client roles");

      case msg_code::REGISTERED:
        process_registered(msg);
        break;

      case msg_code::UNREGISTER:
        throw protocol_error("received UNREGISTER message unexpected for WAMP client roles");

      case msg_code::UNREGISTERED:
        // FIXME
        break;

      case msg_code::INVOCATION:
        process_invocation(msg);
        break;

      case msg_code::INTERRUPT:
        throw protocol_error("received INTERRUPT message - not implemented");

      case msg_code::YIELD:
        throw protocol_error("received YIELD message unexpected for WAMP client roles");
    }
  }

  void Session::send() {

    if (!m_stopped) {
      if (m_debug) {
        qDebug() << "TX message (" << m_buffer.size() << " octets) ..." ;
      }

      qint64 writtenLength = 0;
      qint64 writtenData = 0;

      // write message length prefix
      uint32_t len = htonl(m_buffer.size());
      writtenLength += m_out.write((char*) &len, sizeof(len));
      // write actual serialized message
      char *b = m_buffer.data();
      while((size_t)writtenData < m_buffer.size()) {
        writtenData += m_out.write(&b[writtenData], m_buffer.size() - writtenData);
      }

      if (m_debug) {
        qDebug() << "TX message sent (" << (writtenLength + writtenData) << " / " << (sizeof(len) + m_buffer.size()) << " octets)";
      }
    }
    else if (m_debug) {
      qDebug() << "TX message skipped since session stopped (" << m_buffer.size() << " octets).";
    }

    // clear serialization buffer
    m_buffer.clear();
  }
}
