#ifndef AUTOBAHN_QT
#define AUTOBAHN_QT

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

#include <QMetaMethod>
#include <QVariant>

#include <vector>
#include <map>
#include <functional>

#include <msgpack.hpp>

class QIODevice;

/** \mainpage Reference Documentation
 *
 * Welcome to the reference documentation of <b>Autobahn</b>|Qt.<br>
 * For a more gentle introduction, please visit http://autobahn.ws/qt/.
 */


/**
 * Autobahn namespace.
 */
namespace Autobahn {

  /// Handler type for use with nn::subscribe(const QString&, Handler)
  typedef std::function<void(const QVariantList&, const QVariantMap&)> Handler;

  /// Endpoint type for use with session::provide(const QString&, Endpoint)
  struct Endpoint {
    typedef std::function<QVariant(const QVariantList&, const QVariantMap&)> Function;
    enum Type { Sync, Async };

    Function function;
    Type type;
  };

  /// Represents a procedure registration.
  struct Registration {
      inline Registration(uint64_t id, const QString &procedure) : id(id), procedure(procedure) {}

      uint64_t id;
      QString procedure;
  };

  struct RegisterRequest {
      inline RegisterRequest(const QString &procedure, Endpoint endpoint) : procedure(procedure), endpoint(endpoint) {}

      QString procedure;
      Endpoint endpoint;
  };


  /// Represents a topic subscription.
  struct Subscription {
      inline Subscription(uint64_t id, const QString &topic) : id(id), topic(topic) {}

      uint64_t id;
      QString topic;
  };

  /// An outstanding WAMP subscribe request.
  struct SubscribeRequest {
      inline SubscribeRequest(const QString &topic, Handler handler) : topic(topic), handler(handler) {}

      QString topic;
      Handler handler;
  };

  struct CallRequest {
      inline CallRequest() : ready(false) {}

      bool ready;
      QVariant result;
      std::exception ex;
  };

  /**
   * A WAMP session.
   */
  class Session : public QObject {

      Q_OBJECT

    signals:
      void started();
      void joined(uint64_t);
      void left(QString);
      void subscribed(Subscription);
      void registered(Registration);

    private slots:
      void readData();

    public:

      /**
       * Create a new WAMP session.
       *
       * @param in The input stream to run this session on.
       * @param out The output stream to run this session on.
       */
      Session(QIODevice &in, QIODevice &out, bool debug_calls = false, bool debug = false);

      /**
       * Start listening on the IStream provided to the constructor
       * of this session.
       */
      void start();

      /**
       * Closes the IStream and the OStream provided to the constructor
       * of this session.
       */
      void stop();

      /**
       * Join a realm with this session.
       *
       * @param realm The realm to join on the WAMP router connected to.
       * @return A future that resolves with the session ID when the realm was joined.
       */
      void join(const QString &realm);

      inline bool isJoined() const  { return mIsJoined; }

      /**
       * Leave the realm.
       *
       * @param reason An optional WAMP URI providing a reason for leaving.
       * @return A future that resolves with the reason sent by the peer.
       */
      void leave(const QString &reason = QString("wamp.error.close_realm"));


      /**
       * Subscribe a handler to a topic to receive events.
       *
       * @param topic The URI of the topic to subscribe to.
       * @param handler The handler that will receive events under the subscription.
       * @return A future that resolves to a autobahn::subscription
       */
      void subscribe(const QString &topic, Handler handler);


      /**
       * Calls a remote procedure with no arguments.
       *
       * @param procedure The URI of the remote procedure to call.
       * @return A future that resolves to the result of the remote procedure call.
       */
      QVariant call(const QString& procedure);

      /**
       * Calls a remote procedure with positional arguments.
       *
       * @param procedure The URI of the remote procedure to call.
       * @param args The positional arguments for the call.
       * @return A future that resolves to the result of the remote procedure call.
       */
      QVariant call(const QString& procedure, const QVariantList& args);

      /**
       * Calls a remote procedure with positional and keyword arguments.
       *
       * @param procedure The URI of the remote procedure to call.
       * @param args The positional arguments for the call.
       * @param kwargs The keyword arguments for the call.
       * @return A future that resolves to the result of the remote procedure call.
       */
      QVariant call(const QString& procedure, const QVariantList& args, const QVariantMap& kwargs);


      /**
       * Register an endpoint as a procedure that can be called remotely.
       *
       * @param procedure The URI under which the procedure is to be exposed.
       * @param endpoint The endpoint to be exposed as a remotely callable procedure.
       * @param options Options when registering a procedure.
       */
      void provide(const QString& procedure, Endpoint::Function endpointFunction, Endpoint::Type endpointType = Endpoint::Sync, const QVariantMap& options = QVariantMap());

    public Q_SLOTS:
      /**
       * Publish an event with empty payload to a topic.
       *
       * @param topic The URI of the topic to publish to.
       */
      void publish(const QString &topic);

      /**
       * Publish an event with positional payload to a topic.
       *
       * @param topic The URI of the topic to publish to.
       * @param args The positional payload for the event.
       */
      void publish(const QString &topic, const QVariantList &args);

      /**
       * Publish an event with both positional and keyword payload to a topic.
       *
       * @param topic The URI of the topic to publish to.
       * @param args The positional payload for the event.
       * @param kwargs The keyword payload for the event.
       */
      void publish(const QString& topic, const QVariantList &args, const QVariantMap &kwargs);

    private:

      QVariant makeCall(const QString& procedure, std::function<void()> paramCallback = 0);

      /// Map of outstanding WAMP calls (request ID -> call).
      typedef QMap<uint64_t, CallRequest> CallRequests;

      /// Map of WAMP call ID -> call
      CallRequests callRequests;


      /// Map of outstanding WAMP subscribe requests (request ID -> subscribe request).
      typedef QMap<uint64_t, SubscribeRequest> SubscribeRequests;

      /// Map of WAMP subscribe request ID -> subscribe request
      SubscribeRequests subscribeRequests;

      /// Map of subscribed handlers (subscription ID -> handler)
      typedef QMultiMap<uint64_t, Handler> Handlers;

      /// Map of WAMP subscription ID -> handler
      Handlers handlers;


      //////////////////////////////////////////////////////////////////////////////////////
      /// Callee

      /// An outstanding WAMP register request.

      /// Map of outstanding WAMP register requests (request ID -> register request).
      typedef QMap<uint64_t, RegisterRequest> RegisterRequests;

      /// Map of WAMP register request ID -> register request
      RegisterRequests registerRequests;

      typedef QMap<uint64_t, Endpoint> Endpoints;
      /// Map of WAMP registration ID -> endpoint
      Endpoints endpoints;

      /// An unserialized, raw WAMP message.
      typedef std::vector<msgpack::object> wamp_msg_t;

      /// Process a WAMP ERROR message.
      void process_error(const wamp_msg_t& msg);

      /// Process a WAMP HELLO message.
      void process_welcome(const wamp_msg_t &msg);

      /// Process a WAMP RESULT message.
      void process_call_result(const wamp_msg_t &msg);

      /// Process a WAMP SUBSCRIBED message.
      void process_subscribed(const wamp_msg_t &msg);

      /// Process a WAMP EVENT message.
      void process_event(const wamp_msg_t &msg);

      /// Process a WAMP REGISTERED message.
      void process_registered(const wamp_msg_t &msg);

      /// Process a WAMP INVOCATION message.
      void process_invocation(const wamp_msg_t &msg);

      /// Process a WAMP GOODBYE message.
      void process_goodbye(const wamp_msg_t &msg);


      /// Unpacks any MsgPack object into QVariant value.
      QVariant unpackMsg(msgpack::object& obj);
      QVariantList unpackMsg(std::vector<msgpack::object> &v);
      QVariantMap unpackMsg(std::map<std::string, msgpack::object> &m);

      /// Pack any value into serializion buffer.
      void packQVariant(const QVariant& value);
      void packQVariant(const QVariantList& value);
      void packQVariant(const QVariantMap& value);

      /// Send out message serialized in serialization buffer to ostream.
      void send();

      void get_handshake_reply();
      void get_msg_header();
      void get_msg_body();

      void got_msg(const msgpack::object& obj);

      bool m_debug_calls;
      bool m_debug;
      bool m_stopped;

      /// Input stream this session runs on.
      QIODevice &m_in;

      /// Output stream this session runs on.
      QIODevice &m_out;

      bool mIsJoined;
      char m_buffer_msg_len[4];
      uint32_t m_msg_len;
      uint32_t m_msg_read;

      /// MsgPack serialization buffer.
      msgpack::sbuffer m_buffer;

      /// MsgPacker serialization packer.
      msgpack::packer<msgpack::sbuffer> m_packer;

      /// MsgPack unserialization unpacker.
      msgpack::unpacker m_unpacker;

      /// WAMP session ID (if the session is joined to a realm).
      uint64_t m_session_id;

      /// Last request ID of outgoing WAMP requests.
      uint64_t m_request_id;

      bool m_goodbye_sent;

      enum State {
        Initial,
        Started,
        ReadingMessage
      };

      State state;

      /// WAMP message type codes.
      enum class msg_code : int {
        HELLO = 1,
        WELCOME = 2,
        ABORT = 3,
        CHALLENGE = 4,
        AUTHENTICATE = 5,
        GOODBYE = 6,
        HEARTBEAT = 7,
        ERROR = 8,
        PUBLISH = 16,
        PUBLISHED = 17,
        SUBSCRIBE = 32,
        SUBSCRIBED = 33,
        UNSUBSCRIBE = 34,
        UNSUBSCRIBED = 35,
        EVENT = 36,
        CALL = 48,
        CANCEL = 49,
        RESULT = 50,
        REGISTER = 64,
        REGISTERED = 65,
        UNREGISTER = 66,
        UNREGISTERED = 67,
        INVOCATION = 68,
        INTERRUPT = 69,
        YIELD = 70
      };
  };

  class protocol_error : public std::runtime_error {
    public:
      inline protocol_error(const std::string &msg) : std::runtime_error(msg) {}
  };

  class no_session_error : public std::runtime_error {
    public:
      inline no_session_error() : std::runtime_error("session not joined") {}
  };

}

#endif // AUTOBAHN_QT

