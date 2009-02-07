#include <config.h>
#include <view.h>
#include <controller.h>
#include <configparser.h>
#include <configcontainer.h>
#include <exceptions.h>
#include <downloadthread.h>
#include <colormanager.h>
#include <logger.h>
#include <utils.h>
#include <stflpp.h>
#include <exception.h>
#include <formatstring.h>
#include <regexmanager.h>
#include <rss_parser.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <cerrno>
#include <algorithm>
#include <functional>

#include <sys/time.h>
#include <ctime>
#include <cassert>
#include <signal.h>
#include <sys/utsname.h>
#include <langinfo.h>
#include <libgen.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pwd.h>

#include <ncursesw/ncurses.h>

#include <libxml/xmlversion.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlsave.h>
#include <libxml/uri.h>
#include <curl/curl.h>

namespace newsbeuter {

#define LOCK_SUFFIX ".lock"

static std::string lock_file;

void ctrl_c_action(int sig) {
	LOG(LOG_DEBUG,"caugh signal %d",sig);
	stfl::reset();
	utils::remove_fs_lock(lock_file);
	::exit(EXIT_FAILURE);
}

void ignore_signal(int sig) {
	LOG(LOG_WARN, "caught signal %d but ignored it", sig);
}

void omg_a_child_died(int /* sig */) {
	pid_t pid;
	int stat;
	while ((pid = waitpid(-1,&stat,WNOHANG)) > 0) { }
}

controller::controller() : v(0), urlcfg(0), rsscache(0), url_file("urls"), cache_file("cache.db"), config_file("config"), queue_file("queue"), refresh_on_start(false) {
	char * cfgdir;
	if (!(cfgdir = ::getenv("HOME"))) {
		struct passwd * spw = ::getpwuid(::getuid());
		if (spw) {
			cfgdir = spw->pw_dir;
		} else {
			std::cout << _("Fatal error: couldn't determine home directory!") << std::endl;
			std::cout << utils::strprintf(_("Please set the HOME environment variable or add a valid user for UID %u!"), ::getuid()) << std::endl;
			::exit(EXIT_FAILURE);
		}
	}
	config_dir = cfgdir;

	config_dir.append(NEWSBEUTER_PATH_SEP);
	config_dir.append(NEWSBEUTER_CONFIG_SUBDIR);
	mkdir(config_dir.c_str(),0700); // create configuration directory if it doesn't exist
}

controller::~controller() {
	delete rsscache;
	delete urlcfg;

	for (std::vector<std::tr1::shared_ptr<rss_feed> >::iterator it=feeds.begin();it!=feeds.end();it++) {
		(*it)->items().clear();
	}
	feeds.clear();
}

void controller::set_view(view * vv) {
	v = vv;
}

void controller::run(int argc, char * argv[]) {
	int c;

	url_file = config_dir + std::string(NEWSBEUTER_PATH_SEP) + url_file;
	cache_file = config_dir + std::string(NEWSBEUTER_PATH_SEP) + cache_file;
	lock_file = cache_file + LOCK_SUFFIX;
	config_file = config_dir + std::string(NEWSBEUTER_PATH_SEP) + config_file;
	queue_file = config_dir + std::string(NEWSBEUTER_PATH_SEP) + queue_file;

	::signal(SIGINT, ctrl_c_action);
	::signal(SIGPIPE, ignore_signal);
	::signal(SIGHUP, ctrl_c_action);
	::signal(SIGCHLD, omg_a_child_died);

	bool do_import = false, do_export = false, cachefile_given_on_cmdline = false, do_vacuum = false;
	bool offline_mode = false, real_offline_mode = false;
	std::string importfile;
	bool do_read_import = false, do_read_export = false;
	std::string readinfofile;

	bool silent = false;
	bool execute_cmds = false;

	do {
		if((c = ::getopt(argc,argv,"i:erhu:c:C:d:l:vVoxI:E:"))<0)
			continue;
		switch (c) {
			case ':': /* fall-through */
			case '?': /* missing option */
				usage(argv[0]);
				break;
			case 'i':
				if (do_export)
					usage(argv[0]);
				do_import = true;
				silent = true;
				importfile = optarg;
				break;
			case 'r':
				refresh_on_start = true;
				break;
			case 'e':
				if (do_import)
					usage(argv[0]);
				do_export = true;
				silent = true;
				break;
			case 'h':
				usage(argv[0]);
				break;
			case 'u':
				url_file = optarg;
				break;
			case 'c':
				cache_file = optarg;
				lock_file = std::string(cache_file) + LOCK_SUFFIX;
				cachefile_given_on_cmdline = true;
				break;
			case 'C':
				config_file = optarg;
				break;
			case 'v':
				do_vacuum = true;
				break;
			case 'V':
				version_information();
				break;
			case 'o':
				offline_mode = true;
				break;
			case 'x':
				execute_cmds = true;
				silent = true;
				break;
			case 'd': // this is an undocumented debug commandline option!
				GetLogger().set_logfile(optarg);
				break;
			case 'l': // this is an undocumented debug commandline option!
				{
					loglevel level = static_cast<loglevel>(atoi(optarg));
					if (level > LOG_NONE && level <= LOG_DEBUG)
						GetLogger().set_loglevel(level);
				}
				break;
			case 'I':
				if (do_read_export)
					usage(argv[0]);
				do_read_import = true;
				readinfofile = optarg;
				break;
			case 'E':
				if (do_read_import)
					usage(argv[0]);
				do_read_export = true;
				readinfofile = optarg;
				break;
			default:
				std::cout << utils::strprintf(_("%s: unknown option - %c"), argv[0], static_cast<char>(c)) << std::endl;
				usage(argv[0]);
				break;
		}
	} while (c != -1);


	if (do_import) {
		LOG(LOG_INFO,"Importing OPML file from %s",importfile.c_str());
		urlcfg = new file_urlreader(url_file);
		import_opml(importfile.c_str());
		return;
	}


	LOG(LOG_INFO, "nl_langinfo(CODESET): %s", nl_langinfo(CODESET));

	if (!do_export) {

		if (!silent)
			std::cout << utils::strprintf(_("Starting %s %s..."), PROGRAM_NAME, PROGRAM_VERSION) << std::endl;

		pid_t pid;
		if (!utils::try_fs_lock(lock_file, pid)) {
			if (pid > 0) {
				LOG(LOG_ERROR,"an instance is already running: pid = %u",pid);
			} else {
				LOG(LOG_ERROR,"something went wrong with the lock: %s", strerror(errno));
			}
			std::cout << utils::strprintf(_("Error: an instance of %s is already running (PID: %u)"), PROGRAM_NAME, pid) << std::endl;
			return;
		}
	}

	if (!silent)
		std::cout << _("Loading configuration...");
	std::cout.flush();
	
	cfg.register_commands(cfgparser);
	colorman.register_commands(cfgparser);

	keymap keys(KM_NEWSBEUTER);
	cfgparser.register_handler("bind-key",&keys);
	cfgparser.register_handler("unbind-key",&keys);
	cfgparser.register_handler("macro", &keys);

	cfgparser.register_handler("ignore-article",&ign);
	cfgparser.register_handler("always-download",&ign);
	cfgparser.register_handler("reset-unread-on-update",&ign);

	cfgparser.register_handler("define-filter",&filters);
	cfgparser.register_handler("highlight", &rxman);

	try {
		cfgparser.parse("/etc/" PROGRAM_NAME "/config");
		cfgparser.parse(config_file);
	} catch (const configexception& ex) {
		LOG(LOG_ERROR,"an exception occured while parsing the configuration file: %s",ex.what());
		std::cout << ex.what() << std::endl;
		utils::remove_fs_lock(lock_file);
		return;	
	}

	update_config();

	if (!silent)
		std::cout << _("done.") << std::endl;

	// create cache object
	std::string cachefilepath = cfg.get_configvalue("cache-file");
	if (cachefilepath.length() > 0 && !cachefile_given_on_cmdline) {
		cache_file = cachefilepath.c_str();

		// ok, we got another cache file path via the configuration
		// that means we need to remove the old lock file, assemble
		// the new lock file's name, and then try to lock it.
		utils::remove_fs_lock(lock_file);
		lock_file = std::string(cache_file) + LOCK_SUFFIX;

		pid_t pid;
		if (!utils::try_fs_lock(lock_file, pid)) {
			if (pid > 0) {
				LOG(LOG_ERROR,"an instance is already running: pid = %u",pid);
			} else {
				LOG(LOG_ERROR,"something went wrong with the lock: %s", strerror(errno));
			}
			std::cout << utils::strprintf(_("Error: an instance of %s is already running (PID: %u)"), PROGRAM_NAME, pid) << std::endl;
			return;
		}
	}

	if (!silent) {
		std::cout << _("Opening cache...");
		std::cout.flush();
	}
	try {
		rsscache = new cache(cache_file,&cfg);
	} catch (const dbexception& e) {
		std::cout << utils::strprintf(_("Error: opening the cache file `%s' failed: %s"), cache_file.c_str(), e.what()) << std::endl;
		utils::remove_fs_lock(lock_file);
		::exit(EXIT_FAILURE);
	}

	if (!silent) {
		std::cout << _("done.") << std::endl;
	}



	std::string type = cfg.get_configvalue("urls-source");
	if (type == "local") {
		urlcfg = new file_urlreader(url_file);
	} else if (type == "bloglines") {
		urlcfg = new bloglines_urlreader(&cfg);
		real_offline_mode = offline_mode;
	} else if (type == "opml") {
		urlcfg = new opml_urlreader(&cfg);
		real_offline_mode = offline_mode;
	} else {
		LOG(LOG_ERROR,"unknown urls-source `%s'", urlcfg->get_source().c_str());
	}

	if (real_offline_mode) {
		if (!do_export) {
			std::cout << _("Loading URLs from local cache...");
			std::cout.flush();
		}
		urlcfg->set_offline(true);
		urlcfg->get_urls() = rsscache->get_feed_urls();
		if (!do_export) {
			std::cout << _("done.") << std::endl;
		}
	} else {
		if (!do_export && !silent) {
			std::cout << utils::strprintf(_("Loading URLs from %s..."), urlcfg->get_source().c_str());
			std::cout.flush();
		}
		urlcfg->reload();
		if (!do_export && !silent) {
			std::cout << _("done.") << std::endl;
		}
	}

	if (urlcfg->get_urls().size() == 0) {
		LOG(LOG_ERROR,"no URLs configured.");
		std::string msg;
		if (type == "local") {
			msg = utils::strprintf(_("Error: no URLs configured. Please fill the file %s with RSS feed URLs or import an OPML file."), url_file.c_str());
		} else if (type == "bloglines") {
			msg = utils::strprintf(_("It looks like you haven't configured any feeds in your bloglines account. Please do so, and try again."));
		} else if (type == "opml") {
			msg = utils::strprintf(_("It looks like the OPML feed you subscribed contains no feeds. Please fill it with feeds, and try again."));
		} else {
			assert(0); // shouldn't happen
		}
		std::cout << msg << std::endl << std::endl;
		usage(argv[0]);
	}

	if (!do_export && !do_vacuum && !silent)
		std::cout << _("Loading articles from cache...");
	if (do_vacuum)
		std::cout << _("Opening cache...");
	std::cout.flush();


	if (do_vacuum) {
		std::cout << _("done.") << std::endl;
		std::cout << _("Cleaning up cache thoroughly...");
		std::cout.flush();
		rsscache->do_vacuum();
		std::cout << _("done.") << std::endl;
		utils::remove_fs_lock(lock_file);
		return;
	}

	for (std::vector<std::string>::const_iterator it=urlcfg->get_urls().begin(); it != urlcfg->get_urls().end(); ++it) {
		std::tr1::shared_ptr<rss_feed> feed(new rss_feed(rsscache));
		feed->set_rssurl(*it);
		feed->set_tags(urlcfg->get_tags(*it));
		try {
			rsscache->internalize_rssfeed(feed);
		} catch(const dbexception& e) {
			std::cout << _("Error while loading feeds from database: ") << e.what() << std::endl;
			utils::remove_fs_lock(lock_file);
			return;
		}
		feeds.push_back(feed);
	}

	sort_feeds();

	std::vector<std::string> tags = urlcfg->get_alltags();

	if (!do_export && !silent)
		std::cout << _("done.") << std::endl;

	if (do_export) {
		export_opml();
		utils::remove_fs_lock(lock_file);
		return;
	}

	if (do_read_import) {
		LOG(LOG_INFO,"Importing read information file from %s",readinfofile.c_str());
		std::cout << _("Importing list of read articles...");
		std::cout.flush();
		import_read_information(readinfofile);
		std::cout << _("done.") << std::endl;
		return;
	}

	if (do_read_export) {
		LOG(LOG_INFO,"Exporting read information file to %s",readinfofile.c_str());
		std::cout << _("Exporting list of read articles...");
		std::cout.flush();
		export_read_information(readinfofile);
		std::cout << _("done.") << std::endl;
		return;
	}

	if (execute_cmds) {
		execute_commands(argv, optind);
		utils::remove_fs_lock(lock_file);
		return;	
	}

	// if the user wants to refresh on startup via configuration file, then do so,
	// but only if -r hasn't been supplied.
	if (!refresh_on_start && cfg.get_configvalue_as_bool("refresh-on-startup")) {
		refresh_on_start = true;
	}

	// hand over the important objects to the view
	v->set_config_container(&cfg);
	v->set_keymap(&keys);
	v->set_tags(tags);

	// run the view
	v->run();

	std::cout << _("Cleaning up cache...");
	std::cout.flush();
	try {
		rsscache->cleanup_cache(feeds);
		std::cout << _("done.") << std::endl;
	} catch (const dbexception& e) {
		std::cout << _("failed: ") << e.what() << std::endl;
	}

	utils::remove_fs_lock(lock_file);
}

void controller::update_feedlist() {
	v->set_feedlist(feeds);
}

void controller::update_visible_feeds() {
	v->update_visible_feeds(feeds);
}

void controller::catchup_all() {
	try {
		rsscache->catchup_all();
	} catch (const dbexception& e) {
		v->show_error(utils::strprintf(_("Error: couldn't mark all feeds read: %s"), e.what()));
		return;
	}
	for (std::vector<std::tr1::shared_ptr<rss_feed> >::iterator it=feeds.begin();it!=feeds.end();++it) {
		if ((*it)->items().size() > 0) {
			for (std::vector<std::tr1::shared_ptr<rss_item> >::iterator jt=(*it)->items().begin();jt!=(*it)->items().end();++jt) {
				(*jt)->set_unread_nowrite(false);
			}
		}
	}
}

void controller::mark_all_read(unsigned int pos) {
	if (pos < feeds.size()) {
		scope_measure m("controller::mark_all_read");
		std::tr1::shared_ptr<rss_feed> feed = feeds[pos];
		if (feed->rssurl().substr(0,6) == "query:") {
			rsscache->catchup_all(feed);
		} else {
			rsscache->catchup_all(feed->rssurl());
		}
		m.stopover("after rsscache->catchup_all, before iteration over items");
		std::vector<std::tr1::shared_ptr<rss_item> >& items = feed->items();
		std::vector<std::tr1::shared_ptr<rss_item> >::iterator begin = items.begin(), end = items.end();
		if (items.size() > 0) {
			bool notify = items[0]->feedurl() != feed->rssurl();
			LOG(LOG_DEBUG, "controller::mark_all_read: notify = %s", notify ? "yes" : "no");
			for (std::vector<std::tr1::shared_ptr<rss_item> >::iterator it=begin;it!=end;++it) {
				(*it)->set_unread_nowrite_notify(false, notify);
			}
		}
	}
}

void controller::reload(unsigned int pos, unsigned int max, bool unattended) {
	LOG(LOG_DEBUG, "controller::reload: pos = %u max = %u", pos, max);
	if (pos < feeds.size()) {
		std::tr1::shared_ptr<rss_feed> feed = feeds[pos];
		if (!unattended)
			v->set_status(utils::strprintf(_("%sLoading %s..."), prepare_message(pos+1, max).c_str(), utils::censor_url(feed->rssurl()).c_str()));

		rss_parser parser(feed->rssurl().c_str(), rsscache, &cfg, &ign);
		LOG(LOG_DEBUG, "controller::reload: created parser");
		try {
			feed = parser.parse();
			if (feed->items().size() > 0) {
				save_feed(feed, pos);
				enqueue_items(feed);
				if (!unattended)
					v->set_feedlist(feeds);
			}
			v->set_status("");
		} catch (const dbexception& e) {
			v->set_status(utils::strprintf(_("Error while retrieving %s: %s"), utils::censor_url(feed->rssurl()).c_str(), e.what()));
		} catch (const std::string& errmsg) {
			v->set_status(utils::strprintf(_("Error while retrieving %s: %s"), utils::censor_url(feed->rssurl()).c_str(), errmsg.c_str()));
		} catch (rsspp::exception& e) {
			v->set_status(utils::strprintf(_("Error while retrieving %s: %s"), utils::censor_url(feed->rssurl()).c_str(), e.what()));
		}
	} else {
		v->show_error(_("Error: invalid feed!"));
	}
}

std::tr1::shared_ptr<rss_feed> controller::get_feed(unsigned int pos) {
	if (pos >= feeds.size()) {
		throw std::out_of_range(_("invalid feed index (bug)"));
	}
	return feeds[pos];
}

void controller::reload_indexes(const std::vector<int>& indexes, bool unattended) {
	scope_measure m1("controller::reload_indexes");
	unsigned int unread_feeds, unread_articles;
	compute_unread_numbers(unread_feeds, unread_articles);

	for (std::vector<int>::const_iterator it=indexes.begin();it!=indexes.end();++it) {
		this->reload(*it,feeds.size(), unattended);
	}

	unsigned int unread_feeds2, unread_articles2;
	compute_unread_numbers(unread_feeds2, unread_articles2);
	if (unread_feeds2 != unread_feeds || unread_articles2 != unread_articles) {
		fmtstr_formatter fmt;
		fmt.register_fmt('f', utils::to_s(unread_feeds2));
		fmt.register_fmt('n', utils::to_s(unread_articles2));
		fmt.register_fmt('d', utils::to_s(unread_articles2 - unread_articles));
		fmt.register_fmt('D', utils::to_s(unread_feeds2 - unread_feeds));
		this->notify(fmt.do_format(cfg.get_configvalue("notify-format")));
	}
	if (!unattended)
		v->set_status("");
}

void controller::reload_range(unsigned int start, unsigned int end, unsigned int size, bool unattended) {
	for (unsigned int i=start;i<=end;i++) {
		LOG(LOG_DEBUG, "controller::reload_range: reloading feed #%u", i);
		this->reload(i, size, unattended);
	}
}

void controller::reload_all(bool unattended) {
	unsigned int unread_feeds, unread_articles;
	compute_unread_numbers(unread_feeds, unread_articles);
	unsigned int num_threads = cfg.get_configvalue_as_int("reload-threads");
	time_t t1, t2, dt;

	t1 = time(NULL);

	LOG(LOG_DEBUG,"controller::reload_all: starting with reload all...");
	if (num_threads <= 1) {
		this->reload_range(0, feeds.size()-1, feeds.size(), unattended);
	} else {
		std::vector<std::pair<unsigned int, unsigned int> > partitions = utils::partition_indexes(0, feeds.size()-1, num_threads);
		std::vector<pthread_t> threads;
		LOG(LOG_DEBUG, "controller::reload_all: starting reload threads...");
		for (unsigned int i=0;i<num_threads-1;i++) {
			reloadrangethread* t = new reloadrangethread(this, partitions[i].first, partitions[i].second, feeds.size(), unattended);
			threads.push_back(t->start());
		}
		LOG(LOG_DEBUG, "controller::reload_all: starting my own reload...");
		this->reload_range(partitions[num_threads-1].first, partitions[num_threads-1].second, feeds.size(), unattended);
		LOG(LOG_DEBUG, "controller::reload_all: joining other threads...");
		for (std::vector<pthread_t>::iterator it=threads.begin();it!=threads.end();it++) {
			::pthread_join(*it, NULL);
		}
	}

	t2 = time(NULL);
	dt = t2 - t1;
	LOG(LOG_INFO, "controller::reload_all: reload took %d seconds", dt);

	unsigned int unread_feeds2, unread_articles2;
	compute_unread_numbers(unread_feeds2, unread_articles2);
	if (unread_feeds2 != unread_feeds || unread_articles2 != unread_articles) {
		fmtstr_formatter fmt;
		fmt.register_fmt('f', utils::to_s(unread_feeds2));
		fmt.register_fmt('n', utils::to_s(unread_articles2));
		fmt.register_fmt('d', utils::to_s(unread_articles2 - unread_articles));
		fmt.register_fmt('D', utils::to_s(unread_feeds2 - unread_feeds));
		this->notify(fmt.do_format(cfg.get_configvalue("notify-format")));
	}
}

void controller::notify(const std::string& msg) {
	if (cfg.get_configvalue_as_bool("notify-screen")) {
		LOG(LOG_DEBUG, "controller:notify: notifying screen");
		std::cout << "\033^" << msg << "\033\\";
		std::cout.flush();
	}
	if (cfg.get_configvalue_as_bool("notify-xterm")) {
		LOG(LOG_DEBUG, "controller:notify: notifying xterm");
		std::cout << "\033]2;" << msg << "\033\\";
		std::cout.flush();
	}
	if (cfg.get_configvalue("notify-program").length() > 0) {
		std::string prog = cfg.get_configvalue("notify-program");
		LOG(LOG_DEBUG, "controller:notify: notifying external program `%s'", prog.c_str());
		utils::run_command(prog, msg);
	}
}

void controller::compute_unread_numbers(unsigned int& unread_feeds, unsigned int& unread_articles) {
	unread_feeds = 0;
	unread_articles = 0;
	for (std::vector<std::tr1::shared_ptr<rss_feed> >::iterator it=feeds.begin();it!=feeds.end();++it) {
		unsigned int items = (*it)->unread_item_count();
		if (items > 0) {
			++unread_feeds;
			unread_articles += items;
		}
	}
}

bool controller::trylock_reload_mutex() {
	if (reload_mutex.trylock()) {
		LOG(LOG_DEBUG, "controller::trylock_reload_mutex succeeded");
		return true;
	}
	LOG(LOG_DEBUG, "controller::trylock_reload_mutex failed");
	return false;
}

void controller::start_reload_all_thread(std::vector<int> * indexes) {
	LOG(LOG_INFO,"starting reload all thread");
	thread * dlt = new downloadthread(this, indexes);
	dlt->start();
}

void controller::version_information() {
	std::cout << PROGRAM_NAME << " " << PROGRAM_VERSION << " - " << PROGRAM_URL << std::endl;
	std::cout << "Copyright (C) 2006-2009 Andreas Krennmair" << std::endl << std::endl;

	struct utsname xuts;
	uname(&xuts);

	std::cout << "System: " << xuts.sysname << " " << xuts.release << " (" << xuts.machine << ")" << std::endl;
#if defined(__GNUC__) && defined(__VERSION__)
	std::cout << "Compiler: g++ " << __VERSION__ << std::endl;
#endif
	std::cout << "ncurses: " << curses_version() << " (compiled with " << NCURSES_VERSION << ")" << std::endl;
	std::cout << "libcurl: " << curl_version()  << " (compiled with " << LIBCURL_VERSION << ")" << std::endl;
	std::cout << "SQLite: " << sqlite3_libversion() << " (compiled with " << SQLITE_VERSION << ")" << std::endl;
	std::cout << "libxml2: compiled with " << LIBXML_DOTTED_VERSION << std::endl;

	::exit(EXIT_SUCCESS);
}

static unsigned int gentabs(const std::string& str) {
	int tabcount = 2 - ((3 + utils::strwidth(str)) / 8);
	if (tabcount <= 0) {
		tabcount = 1;
	}
	return tabcount;
}

void controller::usage(char * argv0) {
	std::cout << utils::strprintf(_("%s %s\nusage: %s [-i <file>|-e] [-u <urlfile>] [-c <cachefile>] [-x <command> ...] [-h]\n"), 
					PROGRAM_NAME, PROGRAM_VERSION, argv0);
	struct {
		char arg;
		const char * params;
		const char * desc;
	} args[] = {
		{ 'e', "", _("export OPML feed to stdout") },
		{ 'r', "", _("refresh feeds on start") },
		{ 'i', _("<file>"), _("import OPML file") },
		{ 'u', _("<urlfile>"), _("read RSS feed URLs from <urlfile>") },
		{ 'c', _("<cachefile>"), _("use <cachefile> as cache file") },
		{ 'C', _("<configfile>"), _("read configuration from <configfile>") },
		{ 'v', "", _("clean up cache thoroughly") },
		{ 'x', _("<command>..."), _("execute list of commands") },
		{ 'o', "", _("activate offline mode (only applies to bloglines synchronization mode)") },
		{ 'V', "", _("get version information") },
		{ 'l', _("<loglevel>"), _("write a log with a certain loglevel (valid values: 1 to 6)") },
		{ 'd', _("<logfile>"), _("use <logfile> as output log file") },
		{ 'E', _("<file>"), _("export list of read articles to <file>") },
		{ 'I', _("<file>"), _("import list of read articles from <file>") },
		{ 'h', "", _("this help") },
		{ '\0', NULL, NULL }
	};

	for (unsigned int i=0;args[i].arg != '\0';i++) {
		unsigned int tabs = gentabs(args[i].params);
		std::cout << "\t-" << args[i].arg << " " << args[i].params;
		for (unsigned int j=0;j<tabs;j++) { 
			std::cout << "\t";
		}
		std::cout << args[i].desc << std::endl;
	}
	::exit(EXIT_FAILURE);
}

void controller::import_opml(const char * filename) {
	xmlDoc * doc = xmlReadFile(filename, NULL, 0);
	if (doc == NULL) {
		std::cout << utils::strprintf(_("An error occured while parsing %s."), filename) << std::endl;
		return;
	}

	xmlNode * root = xmlDocGetRootElement(doc);

	for (xmlNode * node = root->children; node != NULL; node = node->next) {
		if (strcmp((const char *)node->name, "body")==0) {
			LOG(LOG_DEBUG, "import_opml: found body");
			rec_find_rss_outlines(node->children, "");
			urlcfg->write_config();
		}
	}

	xmlFreeDoc(doc);
	std::cout << utils::strprintf(_("Import of %s finished."), filename) << std::endl;
}

void controller::export_opml() {
	xmlDocPtr root = xmlNewDoc((const xmlChar *)"1.0");
	xmlNodePtr opml_node = xmlNewDocNode(root, NULL, (const xmlChar *)"opml", NULL);
	xmlSetProp(opml_node, (const xmlChar *)"version", (const xmlChar *)"1.0");
	xmlDocSetRootElement(root, opml_node);

	xmlNodePtr head = xmlNewTextChild(opml_node, NULL, (const xmlChar *)"head", NULL);
	xmlNewTextChild(head, NULL, (const xmlChar *)"title", (const xmlChar *)PROGRAM_NAME " - Exported Feeds");
	xmlNodePtr body = xmlNewTextChild(opml_node, NULL, (const xmlChar *)"body", NULL);

	for (std::vector<std::tr1::shared_ptr<rss_feed> >::iterator it=feeds.begin(); it != feeds.end(); ++it) {
		if ((*it)->rssurl().substr(0,6) != "query:" && (*it)->rssurl().substr(0,7) != "filter:") {
			std::string rssurl = (*it)->rssurl();
			std::string link = (*it)->link();
			std::string title = (*it)->title();

			xmlNodePtr outline = xmlNewTextChild(body, NULL, (const xmlChar *)"outline", NULL);
			xmlSetProp(outline, (const xmlChar *)"type", (const xmlChar *)"rss");
			xmlSetProp(outline, (const xmlChar *)"xmlUrl", (const xmlChar *)rssurl.c_str());
			xmlSetProp(outline, (const xmlChar *)"htmlUrl", (const xmlChar *)link.c_str());
			xmlSetProp(outline, (const xmlChar *)"title", (const xmlChar *)title.c_str());
		}
	}

	xmlSaveCtxtPtr savectx = xmlSaveToFd(1, NULL, 1);
	xmlSaveDoc(savectx, root);
	xmlSaveClose(savectx);

	xmlFreeNode(opml_node);
}

void controller::rec_find_rss_outlines(xmlNode * node, std::string tag) {
	while (node) {
		std::string newtag = tag;


		if (strcmp((const char *)node->name, "outline")==0) {
			char * url = (char *)xmlGetProp(node, (const xmlChar *)"xmlUrl");
			if (!url) {
				url = (char *)xmlGetProp(node, (const xmlChar *)"url");
			}

			if (url) {
				LOG(LOG_DEBUG,"OPML import: found RSS outline with url = %s",url);

				bool found = false;

				LOG(LOG_DEBUG, "OPML import: size = %u", urlcfg->get_urls().size());
				if (urlcfg->get_urls().size() > 0) {
					for (std::vector<std::string>::iterator it = urlcfg->get_urls().begin(); it != urlcfg->get_urls().end(); ++it) {
						if (*it == url) {
							found = true;
						}
					}
				}

				if (!found) {
					LOG(LOG_DEBUG,"OPML import: added url = %s",url);
					urlcfg->get_urls().push_back(std::string(url));
					if (tag.length() > 0) {
						LOG(LOG_DEBUG, "OPML import: appending tag %s to url %s", tag.c_str(), url);
						urlcfg->get_tags(url).push_back(tag);
					}
				} else {
					LOG(LOG_DEBUG,"OPML import: url = %s is already in list",url);
				}
				xmlFree(url);
			} else {
				char * text = (char *)xmlGetProp(node, (const xmlChar *)"text");
				if (!text)
					text = (char *)xmlGetProp(node, (const xmlChar *)"title");
				if (text) {
					if (newtag.length() > 0) {
						newtag.append("/");
					}
					newtag.append(text);
					xmlFree(text);
				}
			}
		}
		rec_find_rss_outlines(node->children, newtag);

		node = node->next;
	}
}



std::vector<std::tr1::shared_ptr<rss_item> > controller::search_for_items(const std::string& query, const std::string& feedurl) {
	std::vector<std::tr1::shared_ptr<rss_item> > items = rsscache->search_for_items(query, feedurl);
	LOG(LOG_DEBUG, "controller::search_for_items: setting feed pointers");
	for (std::vector<std::tr1::shared_ptr<rss_item> >::iterator it=items.begin();it!=items.end();++it) {
		(*it)->set_feedptr(get_feed_by_url((*it)->feedurl()));
	}
	return items;
}

std::tr1::shared_ptr<rss_feed> controller::get_feed_by_url(const std::string& feedurl) {
	for (std::vector<std::tr1::shared_ptr<rss_feed> >::iterator it=feeds.begin();it!=feeds.end();++it) {
		if (feedurl == (*it)->rssurl())
			return *it;
	}
	return std::tr1::shared_ptr<rss_feed>();
}

bool controller::is_valid_podcast_type(const std::string& /* mimetype */) {
	return true;
}

void controller::enqueue_url(const std::string& url, std::tr1::shared_ptr<rss_feed> feed) {
	bool url_found = false;
	std::fstream f;
	f.open(queue_file.c_str(), std::fstream::in);
	if (f.is_open()) {
		do {
			std::string line;
			getline(f, line);
			if (!f.eof() && line.length() > 0) {
				std::vector<std::string> fields = utils::tokenize_quoted(line);
				if (fields.size() > 0 && fields[0] == url) {
					url_found = true;
					break;
				}
			}
		} while (!f.eof());
		f.close();
	}
	if (!url_found) {
		f.open(queue_file.c_str(), std::fstream::app | std::fstream::out);
		std::string filename = generate_enqueue_filename(url, feed);
		f << url << " " << stfl::quote(filename) << std::endl;
		f.close();
	}
}

void controller::reload_urls_file() {
	urlcfg->reload();
	std::vector<std::tr1::shared_ptr<rss_feed> > new_feeds;

	for (std::vector<std::string>::const_iterator it=urlcfg->get_urls().begin();it!=urlcfg->get_urls().end();++it) {
		bool found = false;
		for (std::vector<std::tr1::shared_ptr<rss_feed> >::iterator jt=feeds.begin();jt!=feeds.end();++jt) {
			if (*it == (*jt)->rssurl()) {
				found = true;
				(*jt)->set_tags(urlcfg->get_tags(*it));
				new_feeds.push_back(*jt);
				break;
			}
		}
		if (!found) {
			std::tr1::shared_ptr<rss_feed> new_feed(new rss_feed(rsscache));
			new_feed->set_rssurl(*it);
			new_feed->set_tags(urlcfg->get_tags(*it));
			try {
				rsscache->internalize_rssfeed(new_feed);
			} catch(const dbexception& e) {
				LOG(LOG_ERROR, "controller::reload_urls_file: caught exception: %s", e.what());
				throw e;
			}
			new_feeds.push_back(new_feed);
		}
	}

	feeds = new_feeds;

	sort_feeds();

	update_feedlist();
}

void controller::edit_urls_file() {
	const char * editor = getenv("EDITOR");
	if (!editor)
		editor = "vi";

	std::string cmdline = utils::strprintf("%s \"%s\"", editor, utils::replace_all(url_file,"\"","\\\"").c_str());

	v->push_empty_formaction();
	stfl::reset();

	LOG(LOG_DEBUG, "controller::edit_urls_file: running `%s'", cmdline.c_str());
	::system(cmdline.c_str());

	v->pop_current_formaction();

	reload_urls_file();
}

void controller::set_feedptrs(std::tr1::shared_ptr<rss_feed> feed) {
	for (std::vector<std::tr1::shared_ptr<rss_item> >::iterator it=feed->items().begin();it!=feed->items().end();++it) {
		(*it)->set_feedptr(feed);
	}
}

std::string controller::bookmark(const std::string& url, const std::string& title, const std::string& description) {
	std::string bookmark_cmd = cfg.get_configvalue("bookmark-cmd");
	bool is_interactive = cfg.get_configvalue_as_bool("bookmark-interactive");
	if (bookmark_cmd.length() > 0) {
		std::string cmdline = utils::strprintf("%s '%s' %s %s", 
			bookmark_cmd.c_str(), utils::replace_all(url,"'", "%27").c_str(), 
			stfl::quote(title).c_str(), stfl::quote(description).c_str());

		LOG(LOG_DEBUG, "controller::bookmark: cmd = %s", cmdline.c_str());

		if (is_interactive) {
			v->push_empty_formaction();
			stfl::reset();
			::system(cmdline.c_str());
			v->pop_current_formaction();
			return "";
		} else {
			char * my_argv[4];
			my_argv[0] = const_cast<char *>("/bin/sh");
			my_argv[1] = const_cast<char *>("-c");
			my_argv[2] = const_cast<char *>(cmdline.c_str());
			my_argv[3] = NULL;
			return utils::run_program(my_argv, "");
		}
	} else {
		return _("bookmarking support is not configured. Please set the configuration variable `bookmark-cmd' accordingly.");
	}
}

void controller::execute_commands(char ** argv, unsigned int i) {
	if (v->formaction_stack_size() > 0)
		v->pop_current_formaction();
	for (;argv[i];++i) {
		LOG(LOG_DEBUG, "controller::execute_commands: executing `%s'", argv[i]);
		std::string cmd(argv[i]);
		if (cmd == "reload") {
			reload_all(true);
		} else if (cmd == "print-unread") {
			std::cout << utils::strprintf(_("%u unread articles"), rsscache->get_unread_count()) << std::endl;
		}
	}
}

std::string controller::write_temporary_item(std::tr1::shared_ptr<rss_item> item) {
	char filename[1024];
	snprintf(filename, sizeof(filename), "/tmp/newsbeuter-article.XXXXXX");
	int fd = mkstemp(filename);
	if (fd != -1) {
		write_item(item, filename);
		return std::string(filename);
	} else {
		return "";
	}
}

void controller::write_item(std::tr1::shared_ptr<rss_item> item, const std::string& filename) {
	std::fstream f;
	f.open(filename.c_str(),std::fstream::out);
	if (!f.is_open())
		throw exception(errno);

	write_item(item, f);
}

void controller::write_item(std::tr1::shared_ptr<rss_item> item, std::ostream& ostr) {
	std::vector<std::string> lines;
	std::vector<linkpair> links; // not used
	
	std::string title(_("Title: "));
	title.append(item->title());
	lines.push_back(title);
	
	std::string author(_("Author: "));
	author.append(item->author());
	lines.push_back(author);
	
	std::string date(_("Date: "));
	date.append(item->pubDate());
	lines.push_back(date);

	std::string link(_("Link: "));
	link.append(item->link());
	lines.push_back(link);
	
	lines.push_back(std::string(""));
	
	unsigned int width = cfg.get_configvalue_as_int("text-width");
	if (width == 0)
		width = 80;
	htmlrenderer rnd(width, true);
	rnd.render(item->description(), lines, links, item->feedurl());

	for (std::vector<std::string>::iterator it=lines.begin();it!=lines.end();++it) {
		ostr << *it << std::endl;
	}
}

void controller::mark_deleted(const std::string& guid, bool b) {
	rsscache->mark_item_deleted(guid, b);
}

std::string controller::prepare_message(unsigned int pos, unsigned int max) {
	if (max > 0) {
		return utils::strprintf("(%u/%u) ", pos, max);
	}
	return "";
}

void controller::save_feed(std::tr1::shared_ptr<rss_feed> feed, unsigned int pos) {
	if (!feed->is_empty()) {
		LOG(LOG_DEBUG, "controller::reload: feed is nonempty, saving");
		rsscache->externalize_rssfeed(feed, ign.matches_resetunread(feed->rssurl()));
		LOG(LOG_DEBUG, "controller::reload: after externalize_rssfeed");

		rsscache->internalize_rssfeed(feed);
		LOG(LOG_DEBUG, "controller::reload: after internalize_rssfeed");
		feed->set_tags(urlcfg->get_tags(feed->rssurl()));
		feeds[pos]->items().clear();
		feeds[pos] = feed;
		v->notify_itemlist_change(feeds[pos]);
	} else {
		LOG(LOG_DEBUG, "controller::reload: feed is empty, not saving");
	}
}

void controller::enqueue_items(std::tr1::shared_ptr<rss_feed> feed) {
	if (!cfg.get_configvalue_as_bool("podcast-auto-enqueue"))
		return;
	for (std::vector<std::tr1::shared_ptr<rss_item> >::iterator it=feed->items().begin();it!=feed->items().end();++it) {
		if (!(*it)->enqueued() && (*it)->enclosure_url().length() > 0) {
			LOG(LOG_DEBUG, "controller::reload: enclosure_url = `%s' enclosure_type = `%s'", (*it)->enclosure_url().c_str(), (*it)->enclosure_type().c_str());
			if (is_valid_podcast_type((*it)->enclosure_type())) {
				LOG(LOG_INFO, "controller::reload: enqueuing `%s'", (*it)->enclosure_url().c_str());
				enqueue_url((*it)->enclosure_url(), feed);
				(*it)->set_enqueued(true);
				rsscache->update_rssitem_unread_and_enqueued(*it, feed->rssurl());
			}
		}
	}
}

std::string controller::generate_enqueue_filename(const std::string& url, std::tr1::shared_ptr<rss_feed> feed) {
	std::string dlformat = cfg.get_configvalue("download-path");
	if (dlformat[dlformat.length()-1] != NEWSBEUTER_PATH_SEP[0])
		dlformat.append(NEWSBEUTER_PATH_SEP);

	fmtstr_formatter fmt;
	fmt.register_fmt('n', feed->title());
	fmt.register_fmt('h', get_hostname_from_url(url));

	std::string dlpath = fmt.do_format(dlformat);

	char buf[2048];
	snprintf(buf, sizeof(buf), "%s", url.c_str());
	char * base = basename(buf);
	if (!base || strlen(base) == 0) {
		char lbuf[128];
		time_t t = time(NULL);
		strftime(lbuf, sizeof(lbuf), "%Y-%b-%d-%H%M%S.unknown", localtime(&t));
		dlpath.append(lbuf);
	} else {
		dlpath.append(base);
	}
	return dlpath;
}

std::string controller::get_hostname_from_url(const std::string& url) {
	xmlURIPtr uri = xmlParseURI(url.c_str());
	std::string hostname;
	if (uri) {
		hostname = uri->server;
		xmlFreeURI(uri);
	}
	return hostname;
}

void controller::import_read_information(const std::string& readinfofile) {
	std::vector<std::string> guids;

	std::ifstream f(readinfofile.c_str());
	std::string line;
	getline(f,line);
	if (!f.is_open()) {
		return;
	}
	while (f.is_open() && !f.eof()) {
		guids.push_back(line);
		getline(f, line);
	}
	rsscache->mark_items_read_by_guid(guids);
}

void controller::export_read_information(const std::string& readinfofile) {
	std::vector<std::string> guids = rsscache->get_read_item_guids();

	std::fstream f;
	f.open(readinfofile.c_str(), std::fstream::out);
	if (f.is_open()) {
		for (std::vector<std::string>::iterator it=guids.begin();it!=guids.end();it++) {
			f << *it << std::endl;
		}
	}
}

struct sort_feeds_by_firsttag : public std::binary_function<std::tr1::shared_ptr<rss_feed>, std::tr1::shared_ptr<rss_feed>, bool> {
	sort_feeds_by_firsttag() { }
	bool operator()(std::tr1::shared_ptr<rss_feed> a, std::tr1::shared_ptr<rss_feed> b) {
		if (a->get_firsttag().length() == 0 || b->get_firsttag().length() == 0) {
			return a->get_firsttag().length() > b->get_firsttag().length();
		}
		return strcasecmp(a->get_firsttag().c_str(), b->get_firsttag().c_str()) < 0;
	}
};


void controller::sort_feeds() {
	std::string sortmethod = cfg.get_configvalue("feed-sort-order");
	if (sortmethod == "none") {
		// that's the default, do nothing
	} else if (sortmethod == "firsttag") {
		std::stable_sort(feeds.begin(), feeds.end(), sort_feeds_by_firsttag());
	}
}

void controller::update_config() {
	v->set_regexmanager(&rxman);

	if (colorman.colors_loaded()) {
		v->set_colors(colorman.get_fgcolors(), colorman.get_bgcolors(), colorman.get_attributes());
		v->apply_colors_to_all_formactions();
	}

	if (cfg.get_configvalue("error-log").length() > 0) {
		GetLogger().set_errorlogfile(cfg.get_configvalue("error-log").c_str());
	}

}

void controller::load_configfile(const std::string& filename) {
	if (cfgparser.parse(filename, true)) {
		update_config();
	} else {
		v->show_error(utils::strprintf(_("Error: couldn't open configuration file `%s'!"), filename.c_str()));
	}
}

void controller::dump_config(const std::string& filename) {
	std::vector<std::string> configlines;
	cfg.dump_config(configlines);
	if (v) {
		v->get_keys()->dump_config(configlines);
	}
	ign.dump_config(configlines);
	filters.dump_config(configlines);
	colorman.dump_config(configlines);
	rxman.dump_config(configlines);
	std::fstream f;
	f.open(filename.c_str(), std::fstream::out);
	if (f.is_open()) {
		for (std::vector<std::string>::iterator it=configlines.begin();it!=configlines.end();it++) {
			f << *it << std::endl;
		}
	}
}

}
