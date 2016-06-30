
C++/Qt implementation of WAMP protocol
======================================

This small library is based on initial version of [CppWAMP](https://github.com/ecorm/cppwamp), but moved
to the [Qt](http://qt.io) world.
An event loop from boost was replaced by Qt's event loop as well as all other boost components.
For data exchange is used QVariant as an universal data type.
Library depends on [qmsgpack](https://github.com/romixlab/qmsgpack).

Usage:

    QTcpSocket socket;
    QWamp::Session *session;
    QObject::connect(&socket, &QTcpSocket::connected, [&]() {

      session = new QWamp::Session(sessionName, socket, QWamp::Transport::MsgPack, true);

      QObject::connect(session, &QWamp::Session::joined, [&](qint64 s) {
        qDebug() << "Session joined to realm" << config.realm << "with session ID " << s;

        QVariant result = session->call("sum", { 4, 5, 6 });
        qDebug() << "Sum result is" << result.toInt();

      });

      QObject::connect(session, &QWamp::Session::started, [&]() {
        session->join("realm");
      });
    });

Working example will be supplied later.

