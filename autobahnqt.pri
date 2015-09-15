INCLUDEPATH += $$PWD
DEPENDPATH = $$PWD

debug: LIBS += -L $$PWD/debug
release: LIBS += -L $$PWD/release

debug: QMAKE_RPATHDIR += $$PWD/debug
release: QMAKE_RPATHDIR += $$PWD/release

LIBS += -lautobahnqt
