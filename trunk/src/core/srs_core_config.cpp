/*
The MIT License (MIT)

Copyright (c) 2013 winlin

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <srs_core_config.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
// file operations.
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <vector>

#include <srs_core_error.hpp>

#define FILE_OFFSET(fd) lseek(fd, 0, SEEK_CUR)

int64_t FILE_SIZE(int fd)
{
	int64_t pre = FILE_OFFSET(fd);
	int64_t pos = lseek(fd, 0, SEEK_END);
	lseek(fd, pre, SEEK_SET);
	return pos;
}

#define LF (char)0x0a
#define CR (char)0x0d

bool is_common_space(char ch)
{
	return (ch == ' ' || ch == '\t' || ch == CR || ch == LF);
}

#define CONF_BUFFER_SIZE 4096

SrsFileBuffer::SrsFileBuffer()
{
	fd = -1;
	line = 0;

	pos = last = start = new char[CONF_BUFFER_SIZE];
	end = start + CONF_BUFFER_SIZE;
}

SrsFileBuffer::~SrsFileBuffer()
{
	if (fd > 0) {
		close(fd);
	}
	srs_freepa(start);
}

int SrsFileBuffer::open(const char* filename)
{
	assert(fd == -1);
	
	if ((fd = ::open(filename, O_RDONLY, 0)) < 0) {
		fprintf(stderr, "open conf file error. errno=%d(%s)\n", errno, strerror(errno));
		return ERROR_SYSTEM_CONFIG_INVALID;
	}
	
	line = 1;
	
	return ERROR_SUCCESS;
}

SrsConfDirective::SrsConfDirective()
{
}

SrsConfDirective::~SrsConfDirective()
{
	std::vector<SrsConfDirective*>::iterator it;
	for (it = directives.begin(); it != directives.end(); ++it) {
		SrsConfDirective* directive = *it;
		srs_freep(directive);
	}
	directives.clear();
}

std::string SrsConfDirective::arg0()
{
	if (args.size() > 0) {
		return args.at(0);
	}
	
	return "";
}

std::string SrsConfDirective::arg1()
{
	if (args.size() > 1) {
		return args.at(1);
	}
	
	return "";
}

std::string SrsConfDirective::arg2()
{
	if (args.size() > 2) {
		return args.at(2);
	}
	
	return "";
}

SrsConfDirective* SrsConfDirective::at(int index)
{
	return directives.at(index);
}

SrsConfDirective* SrsConfDirective::get(std::string _name)
{
	std::vector<SrsConfDirective*>::iterator it;
	for (it = directives.begin(); it != directives.end(); ++it) {
		SrsConfDirective* directive = *it;
		if (directive->name == _name) {
			return directive;
		}
	}
	
	return NULL;
}

int SrsConfDirective::parse(const char* filename)
{
	int ret = ERROR_SUCCESS;
	
	SrsFileBuffer buffer;
	
	if ((ret = buffer.open(filename)) != ERROR_SUCCESS) {
		return ret;
	}
	
	return parse_conf(&buffer, parse_file);
}

// see: ngx_conf_parse
int SrsConfDirective::parse_conf(SrsFileBuffer* buffer, SrsDirectiveType type)
{
	int ret = ERROR_SUCCESS;
	
	while (true) {
		std::vector<std::string> args;
		ret = read_token(buffer, args);
		
		/**
		* ret maybe:
		* ERROR_SYSTEM_CONFIG_INVALID 		error.
		* ERROR_SYSTEM_CONFIG_DIRECTIVE		directive terminated by ';' found
		* ERROR_SYSTEM_CONFIG_BLOCK_START	token terminated by '{' found
		* ERROR_SYSTEM_CONFIG_BLOCK_END		the '}' found
		* ERROR_SYSTEM_CONFIG_EOF			the config file is done
		*/
		if (ret == ERROR_SYSTEM_CONFIG_INVALID) {
			return ret;
		}
		if (ret == ERROR_SYSTEM_CONFIG_BLOCK_END) {
			if (type != parse_block) {
				fprintf(stderr, "line %d: unexpected \"}\"\n", buffer->line);
				return ret;
			}
			return ERROR_SUCCESS;
		}
		if (ret == ERROR_SYSTEM_CONFIG_EOF) {
			if (type == parse_block) {
				fprintf(stderr, "line %d: unexpected end of file, expecting \"}\"\n", buffer->line);
				return ret;
			}
			return ERROR_SUCCESS;
		}
		
		if (args.empty()) {
			fprintf(stderr, "line %d: empty directive.\n", buffer->line);
			return ret;
		}
		
		// build directive tree.
		SrsConfDirective* directive = new SrsConfDirective();

		directive->conf_line = buffer->line;
		directive->name = args[0];
		args.erase(args.begin());
		directive->args.swap(args);
		
		directives.push_back(directive);
		
		if (ret == ERROR_SYSTEM_CONFIG_BLOCK_START) {
			if ((ret = directive->parse_conf(buffer, parse_block)) != ERROR_SUCCESS) {
				return ret;
			}
		}
	}
	
	return ret;
}

// see: ngx_conf_read_token
int SrsConfDirective::read_token(SrsFileBuffer* buffer, std::vector<std::string>& args)
{
	int ret = ERROR_SUCCESS;

	char* pstart = buffer->pos;
	int startline = buffer->line;

	bool sharp_comment = false;
	
	bool d_quoted = false;
	bool s_quoted = false;
	
	bool need_space = false;
	bool last_space = true;
	
	while (true) {
		if ((ret = refill_buffer(buffer, d_quoted, s_quoted, startline, pstart)) != ERROR_SUCCESS) {
			if (!args.empty() || !last_space) {
				fprintf(stderr, "line %d: unexpected end of file, expecting ; or \"}\"\n", buffer->line);
				return ERROR_SYSTEM_CONFIG_INVALID;
			}
			return ret;
		}
		
		char ch = *buffer->pos++;
		
		if (ch == LF) {
			buffer->line++;
			sharp_comment = false;
		}
		
		if (sharp_comment) {
			continue;
		}
		
		if (need_space) {
			if (is_common_space(ch)) {
				last_space = true;
				need_space = false;
				continue;
			}
			if (ch == ';') {
				return ERROR_SYSTEM_CONFIG_DIRECTIVE;
			}
			if (ch == '{') {
				return ERROR_SYSTEM_CONFIG_BLOCK_START;
			}
			fprintf(stderr, "line %d: unexpected '%c'\n", buffer->line, ch);
			return ERROR_SYSTEM_CONFIG_INVALID; 
		}
		
		// last charecter is space.
		if (last_space) {
			if (is_common_space(ch)) {
				continue;
			}
			pstart = buffer->pos - 1;
			startline = buffer->line;
			switch (ch) {
				case ';':
					if (args.size() == 0) {
						fprintf(stderr, "line %d: unexpected ';'\n", buffer->line);
						return ERROR_SYSTEM_CONFIG_INVALID;
					}
					return ERROR_SYSTEM_CONFIG_DIRECTIVE;
				case '{':
					if (args.size() == 0) {
						fprintf(stderr, "line %d: unexpected '{'\n", buffer->line);
						return ERROR_SYSTEM_CONFIG_INVALID;
					}
					return ERROR_SYSTEM_CONFIG_BLOCK_START;
				case '}':
					if (args.size() != 0) {
						fprintf(stderr, "line %d: unexpected '}'\n", buffer->line);
						return ERROR_SYSTEM_CONFIG_INVALID;
					}
					return ERROR_SYSTEM_CONFIG_BLOCK_END;
				case '#':
					sharp_comment = 1;
					continue;
				case '"':
					pstart++;
					d_quoted = true;
					last_space = 0;
					continue;
				case '\'':
					pstart++;
					s_quoted = true;
					last_space = 0;
					continue;
				default:
					last_space = 0;
					continue;
			}
		} else {
		// last charecter is not space
			bool found = false;
			if (d_quoted) {
				if (ch == '"') {
					d_quoted = false;
					need_space = true;
					found = true;
				}
			} else if (s_quoted) {
				if (ch == '\'') {
					s_quoted = false;
					need_space = true;
					found = true;
				}
			} else if (is_common_space(ch) || ch == ';' || ch == '{') {
				last_space = true;
				found = 1;
			}
			
			if (found) {
				int len = buffer->pos - pstart;
				char* word = new char[len];
				memcpy(word, pstart, len);
				word[len - 1] = 0;
				
				args.push_back(word);
				srs_freepa(word);
				
				if (ch == ';') {
					return ERROR_SYSTEM_CONFIG_DIRECTIVE;
				}
				if (ch == '{') {
					return ERROR_SYSTEM_CONFIG_BLOCK_START;
				}
			}
		}
	}
	
	return ret;
}

int SrsConfDirective::refill_buffer(SrsFileBuffer* buffer, bool d_quoted, bool s_quoted, int startline, char*& pstart)
{
	int ret = ERROR_SUCCESS;
	
	if (buffer->pos < buffer->last) {
		return ret;
	}
	
	int size = FILE_SIZE(buffer->fd) - FILE_OFFSET(buffer->fd);
	
	if (size <= 0) {
		return ERROR_SYSTEM_CONFIG_EOF;
	}
	
	int len = buffer->pos - buffer->start;
	if (len >= CONF_BUFFER_SIZE) {
		buffer->line = startline;
		
		if (!d_quoted && !s_quoted) {
			fprintf(stderr, "line %d: too long parameter \"%*s...\" started\n", 
				buffer->line, 10, buffer->start);
			
		} else {
			fprintf(stderr, "line %d: too long parameter, "
				"probably missing terminating '%c' character\n", buffer->line, d_quoted? '"':'\'');
		}
		return ERROR_SYSTEM_CONFIG_INVALID;
	}
	
	if (len) {
		memmove(buffer->start, pstart, len);
	}
	
	size = srs_min(size, buffer->end - (buffer->start + len));
	int n = read(buffer->fd, buffer->start + len, size);
	if (n != size) {
		fprintf(stderr, "read file read error. expect %d, actual %d bytes.\n", size, n);
		return ERROR_SYSTEM_CONFIG_INVALID;
	}
	
	buffer->pos = buffer->start + len;
	buffer->last = buffer->pos + n;
	pstart = buffer->start;
	
	return ret;
}

Config* config = new Config();

Config::Config()
{
	show_help = false;
	show_version = false;
	config_file = NULL;
	
	root = new SrsConfDirective();
	root->conf_line = 0;
	root->name = "root";
}

Config::~Config()
{
	srs_freep(root);
}

// see: ngx_get_options
int Config::parse_options(int argc, char** argv)
{
	int ret = ERROR_SUCCESS;
	
	for (int i = 1; i < argc; i++) {
		if ((ret = parse_argv(i, argv)) != ERROR_SUCCESS) {
			return ret;
		}
	}
	
	if (show_help) {
		print_help(argv);
	}
	
	if (show_version) {
		fprintf(stderr, "%s\n", RTMP_SIG_SRS_VERSION);
	}
	
	if (show_help || show_version) {
		exit(0);
	}
	
	if (!config_file) {
		fprintf(stderr, "config file not specified, see help: %s -h\n", argv[0]);
		return ERROR_SYSTEM_CONFIG_INVALID;
	}
	
	if ((ret = root->parse(config_file)) != ERROR_SUCCESS) {
		return ret;
	}
	
	SrsConfDirective* conf = NULL;
	if ((conf = get_listen()) == NULL || conf->args.size() == 0) {
		fprintf(stderr, "line %d: conf error, "
			"directive \"listen\" is empty\n", conf? conf->conf_line:0);
		return ERROR_SYSTEM_CONFIG_INVALID;
	}
	
	return ret;
}

SrsConfDirective* Config::get_vhost(std::string vhost)
{
	srs_assert(root);
	
	for (int i = 0; i < (int)root->directives.size(); i++) {
		SrsConfDirective* conf = root->at(i);
		
		if (conf->name != "vhost") {
			continue;
		}
		
		if (conf->arg0() == vhost) {
			return conf;
		}
	}
	
	if (vhost != RTMP_VHOST_DEFAULT) {
		return get_vhost(RTMP_VHOST_DEFAULT);
	}
	
	return NULL;
}

SrsConfDirective* Config::get_gop_cache(std::string vhost)
{
	SrsConfDirective* conf = get_vhost(vhost);

	if (!conf) {
		return NULL;
	}
	
	return conf->get("gop_cache");
}

SrsConfDirective* Config::get_refer(std::string vhost)
{
	SrsConfDirective* conf = get_vhost(vhost);

	if (!conf) {
		return NULL;
	}
	
	return conf->get("refer");
}

SrsConfDirective* Config::get_refer_play(std::string vhost)
{
	SrsConfDirective* conf = get_vhost(vhost);

	if (!conf) {
		return NULL;
	}
	
	return conf->get("refer_play");
}

SrsConfDirective* Config::get_refer_publish(std::string vhost)
{
	SrsConfDirective* conf = get_vhost(vhost);

	if (!conf) {
		return NULL;
	}
	
	return conf->get("refer_publish");
}

SrsConfDirective* Config::get_listen()
{
	return root->get("listen");
}

int Config::parse_argv(int& i, char** argv)
{
	int ret = ERROR_SUCCESS;

	char* p = argv[i];
		
	if (*p++ != '-') {
		fprintf(stderr, "invalid options(index=%d, value=%s), "
			"must starts with -, see help: %s -h\n", i, argv[i], argv[0]);
		return ERROR_SYSTEM_CONFIG_INVALID;
	}
	
	while (*p) {
		switch (*p++) {
			case '?':
			case 'h':
				show_help = true;
				break;
			case 'v':
			case 'V':
				show_version = true;
				break;
			case 'c':
				if (*p) {
					config_file = p;
					return ret;
				}
				if (argv[++i]) {
					config_file = argv[i];
					return ret;
				}
				fprintf(stderr, "option \"-c\" requires parameter\n");
				return ERROR_SYSTEM_CONFIG_INVALID;
			default:
				fprintf(stderr, "invalid option: \"%c\", see help: %s -h\n", *(p - 1), argv[0]);
				return ERROR_SYSTEM_CONFIG_INVALID;
		}
	}
	
	return ret;
}

void Config::print_help(char** argv)
{
	fprintf(stderr, RTMP_SIG_SRS_NAME" "RTMP_SIG_SRS_VERSION
		" Copyright (c) 2013 winlin\n"
		"Usage: %s [-h?vV] [-c <filename>]\n" 
		"\n"
		"Options:\n"
		"   -?-h            : show help\n"
		"   -v-V            : show version and exit\n"
		"   -c filename     : set configuration file\n"
		"\n"
		RTMP_SIG_SRS_WEB"\n"
		RTMP_SIG_SRS_URL"\n"
		"Email: "RTMP_SIG_SRS_EMAIL"\n",
		argv[0]);
}
