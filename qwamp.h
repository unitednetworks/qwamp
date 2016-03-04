#ifndef QWAMP_H
#define QWAMP_H

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
#include <QTime>
#include <QVariant>

#include <functional>

class QIODevice;

/** \mainpage Reference Documentation
 *
 * Welcome to the reference documentation of <b>Autobahn</b>|Qt.<br>
 * For a more gentle introduction, please visit http://autobahn.ws/qt/.
 */


/**
 * Autobahn namespace.
 */
namespace QWamp {

  struct CallStatistics {
      int callNumber = 0;
      int averageTime = 0;
  };

  QString HmacSHA256(const QString &secret, const QString &key);


  /// Handler type for use with nn::subscribe(const QString&, Handler)
  typedef std::function<void(const QVariantList&, const QVariantMap&)> Handler;

  /// Endpoint type for use with session::provide(const QString&, Endpoint)
  struct Endpoint {
    typedef std::function<QVariant(const QVariantList&, const QVariantMap&)> Function;
    enum Type { Sync, Async };

    Function function;
    Type type;
  };

  typedef std::function<QVariant(const QVariantList &, const QVariantMap &, QWamp::Endpoint::Function)> EndpointWrapper;

  /// Represents a procedure registration.
  struct Registration {
      inline Registration(quint64 id, const QString &procedure) : id(id), procedure(procedure) {}

      quint64 id;
      QString procedure;
  };

  struct RegisterRequest {
      inline RegisterRequest(const QString &procedure, Endpoint endpoint) : procedure(procedure), endpoint(endpoint) {}

      QString procedure;
      Endpoint endpoint;
  };


  /// Represents a topic subscription.
  struct Subscription {
      inline Subscription(quint64 id, const QString &topic) : id(id), topic(topic) {}

      quint64 id;
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
      QString ex;
  };

  struct Challenge {
      QString authid;
      QString authrole;
      QString authmethod;
      QString authprovider;
      QString nonce;
      QDateTime timestamp;
      qint64 session;
  };

  /**
   * A WAMP session.
   */
  class Session : public QObject {

      Q_OBJECT

    Q_SIGNALS:
      void started();
      void joined(quint64);
      void left(QString);
      void subscribed(Subscription);
      void registered(Registration);
      void challenge(const QString &, const QString &, const Challenge &);

    private slots:
      void readData();

    public:

      enum class Transport { Msgpack, Json };
      Q_ENUM(Transport)

      /**
       * Create a new WAMP session.
       *
       * @param in The input stream to run this session on.
       * @param out The output stream to run this session on.
       */
      Session(QIODevice &in, QIODevice &out, Transport transport = Transport::Msgpack, bool debug_calls = false, bool debug = false);

      /**
       * Overloaded previous function with single in/out stream
       *
       * @param inout
       * @param debug_calls
       * @param debug
       */
      Session(QIODevice &inout, Transport transport = Transport::Msgpack, bool debug_calls = false, bool debug = false);

      /**
       * Create a new named WAMP session.
       *
       * @param in The input stream to run this session on.
       * @param out The output stream to run this session on.
       */
      Session(const QString &name, QIODevice &in, QIODevice &out, Transport transport = Transport::Msgpack, bool debug_calls = false, bool debug = false);

      /**
       * Overloaded previous function with single in/out stream
       *
       * @param name
       * @param inout
       * @param debug_calls
       * @param debug
       */
      Session(const QString &name, QIODevice &inout, Transport transport = Transport::Msgpack, bool debug_calls = false, bool debug = false);

      /**
       * Gets session name
       * @return
       */
      const QString &name() const;

      /**
       * Sets the session name (it appears as a prefix in published methods)
       * @param name - name of session
       */
      void setName(const QString &name);

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
      void join(const QString &realm, const QString &authid = QString(), const QStringList &authmethods = QStringList());

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

      void authenticate(const QString &credentials);

      /**
       * Calls a remote procedure with positional and keyword arguments.
       *
       * @param procedure The URI of the remote procedure to call.
       * @param args The positional arguments for the call.
       * @param kwargs The keyword arguments for the call.
       * @return A future that resolves to the result of the remote procedure call.
       */
      QVariant call(const QString &procedure, const QVariantList &args = QVariantList(), const QVariantMap &kwargs = QVariantMap());


      /**
       * Register an endpoint as a procedure that can be called remotely.
       *
       * @param procedure The URI under which the procedure is to be exposed.
       * @param endpoint The endpoint to be exposed as a remotely callable procedure.
       * @param options Options when registering a procedure.
       */
      void provide(const QString& procedure, Endpoint::Function endpointFunction, Endpoint::Type endpointType = Endpoint::Sync, const QVariantMap& options = QVariantMap());
      void provideStatistics();

      const QHash<QString, CallStatistics> &callStatistics() const { return m_callStatistics; }
      inline void setEndpointWrapper(EndpointWrapper w) { endpointWrapper = w; }

    public Q_SLOTS:
      /**
       * Publish an event with both positional and keyword payload to a topic.
       *
       * @param topic The URI of the topic to publish to.
       * @param args The positional payload for the event.
       * @param kwargs The keyword payload for the event.
       */
      void publish(const QString &topic, const QVariantList &args = QVariantList(), const QVariantMap &kwargs = QVariantMap());

    private:
      QString makeName(const QString &name) const;
      bool isUint64(const QVariant &v);
      QVariant convertParam(const QVariant &arg);
      QVariantList convertParams(const QVariantList &args);

      /// Map of outstanding WAMP calls (request ID -> call).
      typedef QHash<quint64, CallRequest> CallRequests;

      /// Map of WAMP call ID -> call
      CallRequests callRequests;


      /// Map of outstanding WAMP subscribe requests (request ID -> subscribe request).
      typedef QHash<quint64, SubscribeRequest> SubscribeRequests;

      /// Map of WAMP subscribe request ID -> subscribe request
      SubscribeRequests subscribeRequests;

      /// Map of subscribed handlers (subscription ID -> handler)
      typedef QMultiHash<quint64, Handler> Handlers;

      /// Map of WAMP subscription ID -> handler
      Handlers handlers;


      //////////////////////////////////////////////////////////////////////////////////////
      /// Callee

      /// An outstanding WAMP register request.

      /// Map of outstanding WAMP register requests (request ID -> register request).
      typedef QHash<quint64, RegisterRequest> RegisterRequests;

      /// Map of WAMP register request ID -> register request
      RegisterRequests registerRequests;

      typedef QHash<quint64, Endpoint> Endpoints;
      /// Map of WAMP registration ID -> endpoint
      Endpoints endpoints;

      /// Process a WAMP ERROR message.
      void process_error(const QVariantList &msg);

      /// Process a WAMP HELLO message.
      void process_welcome(const QVariantList &msg);

      /// Process a WAMP CHALLENGE message.
      void process_challenge(const QVariantList &msg);

      /// Process a WAMP RESULT message.
      void process_call_result(const QVariantList &msg);

      /// Process a WAMP SUBSCRIBED message.
      void process_subscribed(const QVariantList &msg);

      /// Process a WAMP EVENT message.
      void process_event(const QVariantList &msg);

      /// Process a WAMP REGISTERED message.
      void process_registered(const QVariantList &msg);

      /// Process a WAMP INVOCATION message.
      void process_invocation(const QVariantList &msg);

      /// Process a WAMP GOODBYE message.
      void process_goodbye(const QVariantList &msg);


      /// Send out message serialized in serialization buffer to ostream.
      void send(const QVariantList &message);

      void get_handshake_reply();
      void get_msg_header();
      void get_msg_body();

      void got_msg(const QVariant &obj);

      bool m_debug_calls;
      bool m_debug;
      bool m_stopped;

      /// Input stream this session runs on.
      QIODevice &m_in;

      /// Output stream this session runs on.
      QIODevice &m_out;

      bool mIsJoined;
      char m_buffer_msg_len[4];
      quint32 m_msg_len;
      quint32 m_msg_read;

      /// MsgPack serialization buffer.
      QByteArray readBuffer;

      /// WAMP session ID (if the session is joined to a realm).
      quint64 m_session_id;

      /// Last request ID of outgoing WAMP requests.
      quint64 m_request_id;

      bool m_goodbye_sent;
      QString m_name;
      QHash<QString, CallStatistics> m_callStatistics;
      QMap<QString, QStringList> m_methods;

      EndpointWrapper endpointWrapper;
      Transport mTransport;

      enum State {
        Initial,
        Started,
        ReadingMessage
      };

      State state;

      /// WAMP message type codes.
      enum class WampMsgCode : int {
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

  class authorization_error : public std::runtime_error {
    public:
      inline authorization_error(const std::string &msg) : std::runtime_error(msg) {}
  };
}

Q_DECLARE_METATYPE(QWamp::Session::Transport)

#endif // QWAMP_H

