# Copyright (C) 2016 The Qt Company Ltd.
# SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

try:
    from urllib2 import ProxyHandler, build_opener, install_opener, urlopen         # Python 2
except ImportError:
    from urllib.request import ProxyHandler, build_opener, install_opener, urlopen  # Python 3

################ workarounds for issues tracked inside jira #################

JIRA_URL='https://bugreports.qt.io/browse'

class JIRA:
    __instance__ = None

    # Helper class
    class Bug:
        CREATOR = 'QTCREATORBUG'
        SDK = 'QTSDK'
        QT = 'QTBUG'
        QT_QUICKCOMPONENTS = 'QTCOMPONENTS'

    # constructor of JIRA
    def __init__(self, number, bugType=Bug.CREATOR):
        if JIRA.__instance__ == None:
            JIRA.__instance__ = JIRA.__impl(number, bugType)
            setattr(JIRA, '__instance__',  JIRA.__instance__)
        else:
            JIRA.__instance__._bugType = bugType
            JIRA.__instance__._number = number
            JIRA.__instance__.__fetchResolutionFromJira__()

    # overridden to make it possible to use JIRA just like the
    # underlying implementation (__impl)
    def __getattr__(self, attr):
        return getattr(self.__instance__, attr)

    # overridden to make it possible to use JIRA just like the
    # underlying implementation (__impl)
    def __setattr__(self, attr, value):
        return setattr(self.__instance__, attr, value)

    # function to check if the given bug is open or not
    @staticmethod
    def isBugStillOpen(number, bugType=Bug.CREATOR):
        tmpJIRA = JIRA(number, bugType)
        return tmpJIRA.isOpen()

    # implementation of JIRA singleton
    class __impl:
        # constructor of __impl
        def __init__(self, number, bugType):
            self._number = number
            self._bugType = bugType
            self._fix = None
            self._fetchResults_ = {}
            self.__fetchResolutionFromJira__()

        # this function checks the resolution of the given bug
        # and returns True if the bug can still be assumed as 'Open' and False otherwise
        def isOpen(self):
            # handle special cases
            if self._resolution == None:
                return True
            if self._resolution in ('Duplicate', 'Moved', 'Incomplete', 'Cannot Reproduce', 'Invalid'):
                test.warning("Resolution of bug is '%s' - assuming 'Open' for now." % self._resolution,
                             "Please check the bugreport manually and update this test.")
                return True
            return self._resolution not in ('Done', 'Fixed')

        # this function tries to fetch the resolution from JIRA for the given bug
        # if this isn't possible or the lookup is disabled it does only check the internal
        # dict whether a function for the given bug is deposited or not
        def __fetchResolutionFromJira__(self):
            global JIRA_URL
            bug = "%s-%d" % (self._bugType, self._number)
            if bug in self._fetchResults_:
                result = self._fetchResults_[bug]
                self._resolution = result[0]
                self._fix = result[1]
                return
            data = None
            proxy = os.getenv("SYSTEST_PROXY", None)
            try:
                if proxy:
                    proxy = ProxyHandler({'https': proxy})
                    opener = build_opener(proxy)
                    install_opener(opener)
                bugReport = urlopen('%s/%s' % (JIRA_URL, bug))
                data = bugReport.read()
            except:
                data = self.__tryExternalTools__(proxy)
                if data == None:
                    test.warning("Sorry, ssl module missing - cannot fetch data via HTTPS",
                                 "Try to install the ssl module by yourself, or set the python "
                                 "path inside SQUISHDIR/etc/paths.ini to use a python version with "
                                 "ssl support OR install wget or curl to get rid of this warning!")
            if data == None:
                test.fatal("No resolution info for %s" % bug)
                self._resolution = 'Done'
            else:
                data = stringify(data)
                data = data.replace("\r", "").replace("\n", "")
                resPattern = re.compile('<span\s+id="resolution-val".*?>(?P<resolution>.*?)</span>')
                resolution = resPattern.search(data)
                fixVersion = 'None'
                fixPattern = re.compile('<span.*?id="fixfor-val".*?>(?P<fix>.*?)</span>')
                fix = fixPattern.search(data)
                titlePattern = re.compile('title="(?P<title>.*?)"')
                if fix:
                    fix = titlePattern.search(fix.group('fix').strip())
                    if fix:
                        fixVersion = fix.group('title').strip()
                self._fix = fixVersion
                if resolution:
                    self._resolution = resolution.group("resolution").strip()
                else:
                    test.fatal("FATAL: Cannot get resolution of bugreport %s" % bug,
                               "Looks like JIRA has changed.... Please verify!")
                    self._resolution = None
            if self._resolution == None:
                self.__cropAndLog__(data)
            self._fetchResults_.update({bug:[self._resolution, self._fix]})

        # simple helper function - used as fallback if python has no ssl support
        # tries to find curl or wget in PATH and fetches data with it instead of
        # using urllib2
        def __tryExternalTools__(self, proxy=None):
            global JIRA_URL
            if proxy:
                cmdAndArgs = { 'curl':['-k', '--proxy', proxy],
                               'wget':['-qO-']}
            else:
                cmdAndArgs = { 'curl':['-k'], 'wget':['-qO-']}
            for call in cmdAndArgs:
                prog = shutil.which(call)
                if prog:
                    if call == 'wget' and proxy and os.getenv("https_proxy", None) == None:
                        test.warning("Missing environment variable https_proxy for using wget with proxy!")
                    cmdline = [prog] + cmdAndArgs[call]
                    cmdline += ['%s/%s-%d' % (JIRA_URL, self._bugType, self._number)]
                    return getOutputFromCmdline(cmdline)
            return None

        # this function crops multiple whitespaces from fetched and searches for expected
        # ids without using regex
        def __cropAndLog__(self, fetched):
            if fetched == None:
                test.log("None passed to __cropAndLog__()")
                return
            fetched = " ".join(fetched.split())
            resoInd = fetched.find('resolution-val')
            if resoInd == -1:
                test.log("Resolution not found inside fetched data.",
                         "%s[...]" % fetched[:200])
            else:
                test.log("Fetched and cropped data: [...]%s[...]" % fetched[resoInd-20:resoInd+100])
