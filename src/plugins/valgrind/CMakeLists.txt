add_qtc_plugin(Valgrind
  DEPENDS CPlusPlus
  PLUGIN_DEPENDS Core Debugger ProjectExplorer TextEditor
  SOURCES
    callgrind/callgrindabstractmodel.h
    callgrind/callgrindcallmodel.cpp callgrind/callgrindcallmodel.h
    callgrind/callgrindcostitem.cpp callgrind/callgrindcostitem.h
    callgrind/callgrindcycledetection.cpp callgrind/callgrindcycledetection.h
    callgrind/callgrinddatamodel.cpp callgrind/callgrinddatamodel.h
    callgrind/callgrindfunction.cpp callgrind/callgrindfunction.h callgrind/callgrindfunction_p.h
    callgrind/callgrindfunctioncall.cpp callgrind/callgrindfunctioncall.h
    callgrind/callgrindfunctioncycle.cpp callgrind/callgrindfunctioncycle.h
    callgrind/callgrindparsedata.cpp callgrind/callgrindparsedata.h
    callgrind/callgrindparser.cpp callgrind/callgrindparser.h
    callgrind/callgrindproxymodel.cpp callgrind/callgrindproxymodel.h
    callgrind/callgrindstackbrowser.cpp callgrind/callgrindstackbrowser.h
    callgrindcostdelegate.cpp callgrindcostdelegate.h
    callgrindcostview.cpp callgrindcostview.h
    callgrindhelper.cpp callgrindhelper.h
    callgrindnamedelegate.cpp callgrindnamedelegate.h
    callgrindtextmark.cpp callgrindtextmark.h
    callgrindtool.cpp callgrindtool.h
    callgrindvisualisation.cpp callgrindvisualisation.h
    memcheckerrorview.cpp memcheckerrorview.h
    memchecktool.cpp memchecktool.h
    startremotedialog.cpp startremotedialog.h
    valgrind.qrc
    valgrindplugin.cpp
    valgrindprocess.cpp valgrindprocess.h
    valgrindsettings.cpp valgrindsettings.h
    valgrindtr.h
    valgrindutils.cpp valgrindutils.h
    xmlprotocol/announcethread.cpp xmlprotocol/announcethread.h
    xmlprotocol/error.cpp xmlprotocol/error.h
    xmlprotocol/errorlistmodel.cpp xmlprotocol/errorlistmodel.h
    xmlprotocol/frame.cpp xmlprotocol/frame.h
    xmlprotocol/parser.cpp xmlprotocol/parser.h
    xmlprotocol/stack.cpp xmlprotocol/stack.h
    xmlprotocol/stackmodel.cpp xmlprotocol/stackmodel.h
    xmlprotocol/status.cpp xmlprotocol/status.h
    xmlprotocol/suppression.cpp xmlprotocol/suppression.h
)

extend_qtc_plugin(Valgrind
  CONDITION WIN32
  DEFINES UNICODE _UNICODE
)

extend_qtc_plugin(Valgrind
  CONDITION WITH_TESTS
  SOURCES
    valgrindmemcheckparsertest.cpp valgrindmemcheckparsertest.h
    valgrindtestrunnertest.cpp valgrindtestrunnertest.h
  DEFINES
    PARSERTESTS_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/unit_testdata"
    VALGRIND_FAKE_PATH="${PROJECT_BINARY_DIR}/src/tools/valgrindfake"
    TESTRUNNER_SRC_DIR="${PROJECT_SOURCE_DIR}/tests/auto/valgrind/memcheck/testapps"
    TESTRUNNER_APP_DIR="${PROJECT_BINARY_DIR}/tests/auto/valgrind/memcheck/testapps"
)
