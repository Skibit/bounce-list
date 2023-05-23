#include <HalonMTA.h>
#include <list>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <fstream>
#include <string.h>
#include <syslog.h>
#include <pcre.h>
#include <csv.h>

extern char *__progname;

class bouncepatterns
{
	public:
		std::string path;
		bool autoreload = true;
		std::map<std::string, std::map<std::string, std::list<std::pair<pcre*, std::string>>>> grouping_state_pattern;
		std::map<std::string, std::list<std::pair<pcre*, std::string>>> grouping_default_state_pattern;
		std::map<std::string, std::list<std::pair<pcre*, std::string>>> default_grouping_state_pattern;
		std::list<std::pair<pcre*, std::string>> default_grouping_default_state_pattern;
		std::list<pcre*> regexs;
		~bouncepatterns()
		{
			for (auto re : regexs)
				pcre_free(re);
		}
};

void list_open(const std::string& list, const std::string& path, bool autoreload);
std::string list_lookup(const std::string& list, const std::string& grouping, const std::string& state, const std::string& message);
void list_reopen(const std::string& list);
void list_parse(const std::string& path, std::shared_ptr<bouncepatterns> list);

std::mutex listslock;
std::map<std::string, std::shared_ptr<bouncepatterns>> lists;

HALON_EXPORT
int Halon_version()
{
	return HALONMTA_PLUGIN_VERSION;
}

HALON_EXPORT
bool Halon_init(HalonInitContext* hic)
{
	HalonConfig* cfg;
	HalonMTA_init_getinfo(hic, HALONMTA_INIT_CONFIG, nullptr, 0, &cfg, nullptr);

	try {
		auto lists_ = HalonMTA_config_object_get(cfg, "lists");
		if (lists_)
		{
			size_t l = 0;
			HalonConfig* list;
			while ((list = HalonMTA_config_array_get(lists_, l++)))
			{
				const char* id = HalonMTA_config_string_get(HalonMTA_config_object_get(list, "id"), nullptr);
				const char* path = HalonMTA_config_string_get(HalonMTA_config_object_get(list, "path"), nullptr);
				const char* autoreload = HalonMTA_config_string_get(HalonMTA_config_object_get(list, "autoreload"), nullptr);
				if (!id || !path)
					continue;
				list_open(id, path, !autoreload || strcmp(autoreload, "true") == 0);
			}
		}
		return true;
	} catch (const std::runtime_error& e) {
		syslog(LOG_CRIT, "%s", e.what());
		return false;
	}
}

HALON_EXPORT
void Halon_config_reload(HalonConfig* cfg)
{
	for (auto & list : lists)
	{
		listslock.lock();
		if (!list.second->autoreload)
		{
			listslock.unlock();
			continue;
		}
		listslock.unlock();

		try {
			list_reopen(list.first);
		} catch (const std::runtime_error& e) {
			syslog(LOG_CRIT, "%s", e.what());
		}
	}
}

HALON_EXPORT
bool Halon_command_execute(HalonCommandExecuteContext* hcec, size_t argc, const char* argv[], size_t argvl[], char** out, size_t* outlen)
{
	try {
		if (argc > 1 && strcmp(argv[0], "reload") == 0)
		{
			list_reopen(argv[1]);
			*out = strdup("OK");
			return true;
		}
		if (argc > 2 && strcmp(argv[0], "test") == 0)
		{
			std::string t = list_lookup(argv[1], argv[2], argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : "");
			*out = strdup(t.c_str());
			return true;
		}
		throw std::runtime_error("No such command");
	} catch (const std::runtime_error& e) {
		*out = strdup(e.what());
		return false;
	}
}

HALON_EXPORT
void bounce_list(HalonHSLContext* hhc, HalonHSLArguments* args, HalonHSLValue* ret)
{
	HalonHSLValue* x;
	char* id = nullptr;
	char* grouping = nullptr;
	char* state = nullptr;
	char* text = nullptr;
	size_t textlen = 0;

	x = HalonMTA_hsl_argument_get(args, 0);
	if (x && HalonMTA_hsl_value_type(x) == HALONMTA_HSL_TYPE_STRING)
		HalonMTA_hsl_value_get(x, HALONMTA_HSL_TYPE_STRING, &id, nullptr);
	else
	{
		HalonHSLValue* e = HalonMTA_hsl_throw(hhc);
		HalonMTA_hsl_value_set(e, HALONMTA_HSL_TYPE_EXCEPTION, "Bad id parameter", 0);
		return;
	}

	x = HalonMTA_hsl_argument_get(args, 1);
	if (x && HalonMTA_hsl_value_type(x) == HALONMTA_HSL_TYPE_STRING)
		HalonMTA_hsl_value_get(x, HALONMTA_HSL_TYPE_STRING, &text, &textlen);
	else
	{
		HalonHSLValue* e = HalonMTA_hsl_throw(hhc);
		HalonMTA_hsl_value_set(e, HALONMTA_HSL_TYPE_EXCEPTION, "Bad message parameter", 0);
		return;
	}

	x = HalonMTA_hsl_argument_get(args, 2);
	if (x)
	{
		if (HalonMTA_hsl_value_type(x) == HALONMTA_HSL_TYPE_STRING)
			HalonMTA_hsl_value_get(x, HALONMTA_HSL_TYPE_STRING, &grouping, nullptr);
		else if (HalonMTA_hsl_value_type(x) != HALONMTA_HSL_TYPE_NONE)
		{
			HalonHSLValue* e = HalonMTA_hsl_throw(hhc);
			HalonMTA_hsl_value_set(e, HALONMTA_HSL_TYPE_EXCEPTION, "Bad grouping parameter", 0);
			return;
		}
	}

	x = HalonMTA_hsl_argument_get(args, 3);
	if (x)
	{
		if (HalonMTA_hsl_value_type(x) == HALONMTA_HSL_TYPE_STRING)
			HalonMTA_hsl_value_get(x, HALONMTA_HSL_TYPE_STRING, &state, nullptr);
		else if (HalonMTA_hsl_value_type(x) != HALONMTA_HSL_TYPE_NONE)
		{
			HalonHSLValue* e = HalonMTA_hsl_throw(hhc);
			HalonMTA_hsl_value_set(e, HALONMTA_HSL_TYPE_EXCEPTION, "Bad state parameter", 0);
			return;
		}
	}

	try {
		std::string r = list_lookup(id, std::string(text, textlen), grouping ? grouping : "", state ? state : "");
		if (r.empty())
			return;
		HalonMTA_hsl_value_set(ret, HALONMTA_HSL_TYPE_STRING, r.c_str(), r.size());
	} catch (const std::runtime_error& ex) {
		HalonHSLValue* e = HalonMTA_hsl_throw(hhc);
		HalonMTA_hsl_value_set(e, HALONMTA_HSL_TYPE_EXCEPTION, ex.what(), 0);
		return;
	}
}

HALON_EXPORT
bool Halon_hsl_register(HalonHSLRegisterContext* ptr)
{
	HalonMTA_hsl_module_register_function(ptr, "bounce_list", &bounce_list);
	return true;
}

struct csv_parser_ptr
{
	std::vector<std::string> col;
	bouncepatterns* ptr;
	bool error = false;
};

void cb1(void *s, size_t i, void *p)
{
	auto x = (csv_parser_ptr*)p;
	x->col.push_back(std::string((char*)s, i).c_str());
}

void cb2(int c, void *p)
{
	auto x = (csv_parser_ptr*)p;
	if (x->col.size() < 2 || x->col.size() > 4)
	{
		x->error = true;
		return;
	}
	const char* compile_error;
	int eoffset;
	pcre* re = pcre_compile(x->col[0].size() > 2 && x->col[0][0] == '/' && x->col[0][x->col[0].size() - 1] == '/' ? x->col[0].substr(1, x->col[0].size() - 2).c_str() : x->col[0].c_str(), PCRE_CASELESS, &compile_error, &eoffset, nullptr);
	if (!re)
	{
		x->error = true;
		return;
	}
	if (x->col.size() > 2 && !x->col[2].empty())
	{
		if (x->col.size() > 3 && !x->col[3].empty())
			x->ptr->grouping_state_pattern[x->col[2]][x->col[3]].push_back({ re, x->col[1] });
		else
			x->ptr->grouping_default_state_pattern[x->col[2]].push_back({ re, x->col[1] });
	}
	else
	{
		if (x->col.size() > 3 && !x->col[3].empty())
			x->ptr->default_grouping_state_pattern[x->col[3]].push_back({ re, x->col[1] });
		else
			x->ptr->default_grouping_default_state_pattern.push_back({ re, x->col[1] });
	}
	x->ptr->regexs.push_back(re);
}

void list_parse(const std::string& path, std::shared_ptr<bouncepatterns> list)
{
	std::ifstream file(path);
	if (!file.good())
		throw std::runtime_error("Could not open file: " + path);

	csv_parser p;
	csv_parser_ptr ptr;
	ptr.ptr = list.get();
	csv_init(&p, 0);
	std::string line;
	size_t errors_format = 0, errors = 0;
	while (std::getline(file, line))
	{
		ptr.col.clear();
		ptr.error = false;
		if (csv_parse(&p, line.c_str(), line.size(), cb1, cb2, &ptr) != line.size())
			++errors_format;
		csv_fini(&p, cb1, cb2, &ptr);
		if (ptr.error)
			++errors;
	}
	csv_free(&p);
	file.close();
	if (strcmp(__progname, "smtpd") == 0)
		syslog(LOG_INFO, "bounce-list %s loaded: %zu regexes, %zu format-errors, %zu data-errors",
			path.c_str(),
			list->regexs.size(),
			errors_format,
			errors
		);
}

void list_open(const std::string& list, const std::string& path, bool autoreload)
{
	auto bouncelist = std::make_shared<bouncepatterns>();
	bouncelist->path = path;
	bouncelist->autoreload = autoreload;

	list_parse(bouncelist->path, bouncelist);

	listslock.lock();
	lists[list] = bouncelist;
	listslock.unlock();
}

std::string list_lookup(const std::string& list, const std::string& message, const std::string& grouping, const std::string& state)
{
	listslock.lock();
	auto l = lists.find(list);
	if (l == lists.end())
	{
		listslock.unlock();
		throw std::runtime_error("No such list id");
	}
	auto bouncelist = l->second;
	listslock.unlock();

	if (!grouping.empty() && !state.empty())
	{
		auto x = bouncelist->grouping_state_pattern.find(grouping);
		if (x != bouncelist->grouping_state_pattern.end())
		{
			auto x2 = x->second.find(state);
			if (x2 != x->second.end())
			{
				for (const auto & p : x2->second)
				{
					if (p.first)
						return p.second;
				}
			}
		}
	}
	if (!grouping.empty())
	{
		auto x = bouncelist->grouping_default_state_pattern.find(grouping);
		if (x != bouncelist->grouping_default_state_pattern.end())
		{
			for (const auto & p : x->second)
			{
				int rc = pcre_exec(p.first, nullptr, message.c_str(), (int)message.size(), 0, PCRE_PARTIAL | PCRE_NO_UTF8_CHECK, nullptr, 0);
				if (rc == 0)
					return p.second;
			}
		}
	}
	if (!state.empty())
	{
		auto x = bouncelist->default_grouping_state_pattern.find(state);
		if (x != bouncelist->default_grouping_state_pattern.end())
		{
			for (const auto & p : x->second)
			{
				int rc = pcre_exec(p.first, nullptr, message.c_str(), (int)message.size(), 0, PCRE_PARTIAL | PCRE_NO_UTF8_CHECK, nullptr, 0);
				if (rc == 0)
					return p.second;
			}
		}
	}
	for (const auto & p : bouncelist->default_grouping_default_state_pattern)
	{
		int rc = pcre_exec(p.first, nullptr, message.c_str(), (int)message.size(), 0, PCRE_PARTIAL | PCRE_NO_UTF8_CHECK, nullptr, 0);
		if (rc == 0)
			return p.second;
	}
	return "";
}

void list_reopen(const std::string& list)
{
	listslock.lock();
	auto l = lists.find(list);
	if (l == lists.end())
	{
		listslock.unlock();
		throw std::runtime_error("No such list id");
	}
	auto currentbouncelist = l->second;
	listslock.unlock();

	auto bouncelist = std::make_shared<bouncepatterns>();
	bouncelist->path = currentbouncelist->path;
	bouncelist->autoreload = currentbouncelist->autoreload;

	list_parse(bouncelist->path, bouncelist);

	listslock.lock();
	lists[list] = bouncelist;
	listslock.unlock();
}
