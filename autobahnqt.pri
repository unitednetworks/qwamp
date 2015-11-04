INCLUDEPATH += $$PWD

debug: LIBS += -L $$PWD/debug
release: LIBS += -L $$PWD/release

debug: QMAKE_RPATHDIR += -Wl,-rpath=$$PWD/debug
release: QMAKE_RPATHDIR += -Wl,-rpath=$$PWD/release

LIBS += -lautobahnqt
