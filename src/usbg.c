/*
 * Copyright (C) 2013 Linaro Limited
 *
 * Matt Porter <mporter@linaro.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <dirent.h>
#include <errno.h>
#include <usbg/usbg.h>
#include <netinet/ether.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define STRINGS_DIR "strings"
#define CONFIGS_DIR "configs"
#define FUNCTIONS_DIR "functions"

/**
 * @file usbg.c
 * @todo Handle buffer overflows
 * @todo Error checking and return code propagation
 */

struct usbg_state
{
	char path[USBG_MAX_PATH_LENGTH];

	TAILQ_HEAD(ghead, usbg_gadget) gadgets;
};

struct usbg_gadget
{
	char name[USBG_MAX_NAME_LENGTH];
	char path[USBG_MAX_PATH_LENGTH];
	char udc[USBG_MAX_STR_LENGTH];

	TAILQ_ENTRY(usbg_gadget) gnode;
	TAILQ_HEAD(chead, usbg_config) configs;
	TAILQ_HEAD(fhead, usbg_function) functions;
	usbg_state *parent;
};

struct usbg_config
{
	TAILQ_ENTRY(usbg_config) cnode;
	TAILQ_HEAD(bhead, usbg_binding) bindings;
	usbg_gadget *parent;

	char name[USBG_MAX_NAME_LENGTH];
	char path[USBG_MAX_PATH_LENGTH];
};

struct usbg_function
{
	TAILQ_ENTRY(usbg_function) fnode;
	usbg_gadget *parent;

	char name[USBG_MAX_NAME_LENGTH];
	char path[USBG_MAX_PATH_LENGTH];

	usbg_function_type type;
};

struct usbg_binding
{
	TAILQ_ENTRY(usbg_binding) bnode;
	usbg_config *parent;
	usbg_function *target;

	char name[USBG_MAX_NAME_LENGTH];
	char path[USBG_MAX_PATH_LENGTH];
};

/**
 * @var function_names
 * @brief Name strings for supported USB function types
 */
const char *function_names[] =
{
	"gser",
	"acm",
	"obex",
	"ecm",
	"geth",
	"ncm",
	"eem",
	"rndis",
	"phonet",
};

#define ERROR(msg, ...) do {\
                        fprintf(stderr, "%s()  "msg" \n", \
                                __func__, ##__VA_ARGS__);\
                        fflush(stderr);\
                    } while (0)

#define ERRORNO(msg, ...) do {\
                        fprintf(stderr, "%s()  %s: "msg" \n", \
                                __func__, strerror(errno), ##__VA_ARGS__);\
                        fflush(stderr);\
                    } while (0)

/* Insert in string order */
#define INSERT_TAILQ_STRING_ORDER(HeadPtr, HeadType, NameField, ToInsert, NodeField) \
	do { \
		if (TAILQ_EMPTY(HeadPtr) || \
			(strcmp(ToInsert->NameField, TAILQ_FIRST(HeadPtr)->NameField) < 0)) \
			TAILQ_INSERT_HEAD(HeadPtr, ToInsert, NodeField); \
		else if (strcmp(ToInsert->NameField, TAILQ_LAST(HeadPtr, HeadType)->NameField) > 0) \
			TAILQ_INSERT_TAIL(HeadPtr, ToInsert, NodeField); \
		else { \
			typeof(ToInsert) _cur; \
			TAILQ_FOREACH(_cur, HeadPtr, NodeField) { \
				if (strcmp(ToInsert->NameField, _cur->NameField) > 0) \
					continue; \
				TAILQ_INSERT_BEFORE(_cur, ToInsert, NodeField); \
			} \
		} \
	} while (0)

static int usbg_translate_error(int error)
{
	int ret;

	switch (error) {
	case ENOMEM:
		ret = USBG_ERROR_NO_MEM;
		break;
	case EACCES:
		ret = USBG_ERROR_NO_ACCESS;
		break;
	case ENOENT:
	case ENOTDIR:
		ret = USBG_ERROR_NOT_FOUND;
		break;
	case EINVAL:
	case USBG_ERROR_INVALID_PARAM:
		ret = USBG_ERROR_INVALID_PARAM;
		break;
	case EIO:
		ret = USBG_ERROR_IO;
		break;
	default:
		ret = USBG_ERROR_OTHER_ERROR;
	}

	return ret;
}

static int usbg_lookup_function_type(char *name)
{
	int i = 0;
	int max = sizeof(function_names)/sizeof(char *);

	if (!name)
		return -1;

	do {
		if (!strcmp(name, function_names[i]))
			break;
		i++;
	} while (i != max);

	if (i == max)
		i = -1;

	return i;
}

static int bindings_select(const struct dirent *dent)
{
	if (dent->d_type == DT_LNK)
		return 1;
	else
		return 0;
}

static int file_select(const struct dirent *dent)
{
	if ((strcmp(dent->d_name, ".") == 0) || (strcmp(dent->d_name, "..") == 0))
		return 0;
	else
		return 1;
}

static int usbg_read_buf(char *path, char *name, char *file, char *buf)
{
	char p[USBG_MAX_STR_LENGTH];
	FILE *fp;
	int ret = USBG_SUCCESS;

	sprintf(p, "%s/%s/%s", path, name, file);

	fp = fopen(p, "r");
	if (fp) {
		/* Successfully opened */
		if (!fgets(buf, USBG_MAX_STR_LENGTH, fp)) {
			ERROR("read error");
			ret = USBG_ERROR_IO;
		}

		fclose(fp);
	} else {
		/* Set error correctly */
		ret = usbg_translate_error(errno);
	}

	return ret;
}

static int usbg_read_int(char *path, char *name, char *file, int base,
		int *dest)
{
	char buf[USBG_MAX_STR_LENGTH];
	char *pos;
	int ret;

	ret = usbg_read_buf(path, name, file, buf);
	if (ret == USBG_SUCCESS) {
		*dest = strtol(buf, &pos, base);
		if (!pos)
			ret = USBG_ERROR_OTHER_ERROR;
	}

	return ret;
}

#define usbg_read_dec(p, n, f, d)	usbg_read_int(p, n, f, 10, d)
#define usbg_read_hex(p, n, f, d)	usbg_read_int(p, n, f, 16, d)

static int usbg_read_string(char *path, char *name, char *file, char *buf)
{
	char *p = NULL;
	int ret;

	ret = usbg_read_buf(path, name, file, buf);
	/* Check whether read was successful */
	if (ret == USBG_SUCCESS) {
		if ((p = strchr(buf, '\n')) != NULL)
				*p = '\0';
	} else {
		/* Set this as empty string */
		*buf = '\0';
	}

	return ret;
}

static void usbg_write_buf(char *path, char *name, char *file, char *buf)
{
	char p[USBG_MAX_STR_LENGTH];
	FILE *fp;
	int err;

	sprintf(p, "%s/%s/%s", path, name, file);

	fp = fopen(p, "w");
	if (!fp) {
		ERRORNO("%s\n", p);
		return;
	}

	fputs(buf, fp);
	fflush(fp);
	err = ferror(fp);
	fclose(fp);
	
	if (err){
		ERROR("write error");
		return;
	}
}

static void usbg_write_int(char *path, char *name, char *file, int value, char *str)
{
	char buf[USBG_MAX_STR_LENGTH];

	sprintf(buf, str, value);
	usbg_write_buf(path, name, file, buf);
}

#define usbg_write_dec(p, n, f, v)	usbg_write_int(p, n, f, v, "%d\n")
#define usbg_write_hex16(p, n, f, v)	usbg_write_int(p, n, f, v, "0x%04x\n")
#define usbg_write_hex8(p, n, f, v)	usbg_write_int(p, n, f, v, "0x%02x\n")

static inline void usbg_write_string(char *path, char *name, char *file, char *buf)
{
	usbg_write_buf(path, name, file, buf);
}

static inline void usbg_free_binding(usbg_binding *b)
{
	free(b);
}

static inline void usbg_free_function(usbg_function *f)
{
	free(f);
}

static void usbg_free_config(usbg_config *c)
{
	usbg_binding *b;
	while (!TAILQ_EMPTY(&c->bindings)) {
		b = TAILQ_FIRST(&c->bindings);
		TAILQ_REMOVE(&c->bindings, b, bnode);
		usbg_free_binding(b);
	}
	free(c);
}

static void usbg_free_gadget(usbg_gadget *g)
{
	usbg_config *c;
	usbg_function *f;
	while (!TAILQ_EMPTY(&g->configs)) {
		c = TAILQ_FIRST(&g->configs);
		TAILQ_REMOVE(&g->configs, c, cnode);
		usbg_free_config(c);
	}
	while (!TAILQ_EMPTY(&g->functions)) {
		f = TAILQ_FIRST(&g->functions);
		TAILQ_REMOVE(&g->functions, f, fnode);
		usbg_free_function(f);
	}
	free(g);
}

static void usbg_free_state(usbg_state *s)
{
	usbg_gadget *g;
	while (!TAILQ_EMPTY(&s->gadgets)) {
		g = TAILQ_FIRST(&s->gadgets);
		TAILQ_REMOVE(&s->gadgets, g, gnode);
		usbg_free_gadget(g);
	}
	free(s);
}


static void usbg_parse_function_attrs(usbg_function *f,
		usbg_function_attrs *f_attrs)
{
	struct ether_addr *addr;
	char str_addr[40];

	switch (f->type) {
	case F_SERIAL:
	case F_ACM:
	case F_OBEX:
		usbg_read_dec(f->path, f->name, "port_num", &(f_attrs->serial.port_num));
		break;
	case F_ECM:
	case F_SUBSET:
	case F_NCM:
	case F_EEM:
	case F_RNDIS:
		usbg_read_string(f->path, f->name, "dev_addr", str_addr);
		addr = ether_aton(str_addr);
		if (addr)
			f_attrs->net.dev_addr = *addr;

		usbg_read_string(f->path, f->name, "host_addr", str_addr);
		addr = ether_aton(str_addr);
		if(addr)
			f_attrs->net.host_addr = *addr;

		usbg_read_string(f->path, f->name, "ifname", f_attrs->net.ifname);
		usbg_read_dec(f->path, f->name, "qmult", &(f_attrs->net.qmult));
		break;
	case F_PHONET:
		usbg_read_string(f->path, f->name, "ifname", f_attrs->phonet.ifname);
		break;
	default:
		ERROR("Unsupported function type\n");
	}
}

static int usbg_parse_functions(char *path, usbg_gadget *g)
{
	usbg_function *f;
	int i, n;
	int ret = USBG_SUCCESS;

	struct dirent **dent;
	char fpath[USBG_MAX_PATH_LENGTH];

	sprintf(fpath, "%s/%s/%s", path, g->name, FUNCTIONS_DIR);

	n = scandir(fpath, &dent, file_select, alphasort);
	if (n >= 0) {
		for (i = 0; i < n; i++) {
			if (ret == USBG_SUCCESS) {
				f = malloc(sizeof(usbg_function));
				if (f) {
					f->parent = g;
					strcpy(f->name, dent[i]->d_name);
					strcpy(f->path, fpath);
					f->type = usbg_lookup_function_type(
							strtok(dent[i]->d_name, "."));
					TAILQ_INSERT_TAIL(&g->functions, f, fnode);
				} else {
					ret = USBG_ERROR_NO_MEM;
				}
			}
			free(dent[i]);
		}
		free(dent);
	} else {
		ret = usbg_translate_error(errno);
	}

	return ret;
}

static void usbg_parse_config_attrs(char *path, char *name,
		usbg_config_attrs *c_attrs)
{
	int buf;
	usbg_read_dec(path, name, "MaxPower", &buf);
	c_attrs->bMaxPower = (uint8_t)buf;
	usbg_read_hex(path, name, "bmAttributes", &buf);
	c_attrs->bmAttributes = (uint8_t)buf;
}

static usbg_config_strs *usbg_parse_config_strs(char *path, char *name,
		int lang, usbg_config_strs *c_strs)
{
	DIR *dir;
	char spath[USBG_MAX_PATH_LENGTH];

	sprintf(spath, "%s/%s/%s/0x%x", path, name, STRINGS_DIR, lang);

	/* Check if directory exist */
	dir = opendir(spath);
	if (dir) {
		closedir(dir);
		usbg_read_string(spath, "", "configuration", c_strs->configuration);
	} else {
		c_strs = NULL;
	}

	return c_strs;
}

static int usbg_parse_config_bindings(usbg_config *c)
{
	int i, n, nmb;
	int ret = USBG_SUCCESS;
	struct dirent **dent;
	char bpath[USBG_MAX_PATH_LENGTH];
	char file_name[USBG_MAX_PATH_LENGTH];
	char target[USBG_MAX_STR_LENGTH];
	char *target_name;
	usbg_gadget *g = c->parent;
	usbg_binding *b;
	usbg_function *f;

	sprintf(bpath, "%s/%s", c->path, c->name);

	n = scandir(bpath, &dent, bindings_select, alphasort);
	if (n >= 0) {
		for (i = 0; i < n; i++) {
			sprintf(file_name, "%s/%s", bpath, dent[i]->d_name);
			nmb = readlink(file_name, target, USBG_MAX_PATH_LENGTH);
			if (nmb >= 0) {
				/* readlink() don't add this,
				 * so we have to do it manually */
				target[nmb] = '\0';
				/* Target contains a full path
				 * but we need only function dir name */
				target_name = strrchr(target, '/') + 1;

				f = usbg_get_function(g, target_name);

				b = malloc(sizeof(usbg_binding));
				if (b) {
					strcpy(b->name, dent[i]->d_name);
					strcpy(b->path, bpath);
					b->target = f;
				b->parent = c;
					TAILQ_INSERT_TAIL(&c->bindings, b, bnode);
				} else {
					ret = USBG_ERROR_NO_MEM;
				}
			} else {
				ret = usbg_translate_error(errno);
			}

			free(dent[i]);
		}
		free(dent);
	} else {
		ret = usbg_translate_error(errno);
	}

	return ret;
}

static int usbg_parse_configs(char *path, usbg_gadget *g)
{
	usbg_config *c;
	int i, n;
	int ret = USBG_SUCCESS;
	struct dirent **dent;
	char cpath[USBG_MAX_PATH_LENGTH];

	sprintf(cpath, "%s/%s/%s", path, g->name, CONFIGS_DIR);

	n = scandir(cpath, &dent, file_select, alphasort);
	if (n >= 0) {
		for (i = 0; i < n; i++) {
			if (ret == USBG_SUCCESS) {
				c = malloc(sizeof(usbg_config));
				if (c) {
					c->parent = g;
					strcpy(c->name, dent[i]->d_name);
					strcpy(c->path, cpath);
					TAILQ_INIT(&c->bindings);
					ret = usbg_parse_config_bindings(c);
					if (ret == USBG_SUCCESS)
						TAILQ_INSERT_TAIL(&g->configs, c, cnode);
					else
						usbg_free_config(c);
				} else {
					ret = USBG_ERROR_NO_MEM;
				}
			}
			free(dent[i]);
		}
		free(dent);
	} else {
		ret = usbg_translate_error(errno);
	}

	return ret;
}

static int usbg_parse_gadget_attrs(char *path, char *name,
		usbg_gadget_attrs *g_attrs)
{
	int buf, ret;

	/* Actual attributes */

	ret = usbg_read_hex(path, name, "bcdUSB", &buf);
	if (ret == USBG_SUCCESS)
		g_attrs->bcdUSB = (uint16_t) buf;
	else
		goto out;

	ret = usbg_read_hex(path, name, "bDeviceClass", &buf);
	if (ret == USBG_SUCCESS)
		g_attrs->bDeviceClass = (uint8_t)buf;
	else
		goto out;

	ret = usbg_read_hex(path, name, "bDeviceSubClass", &buf);
	if (ret == USBG_SUCCESS)
		g_attrs->bDeviceSubClass = (uint8_t)buf;
	else
		goto out;

	ret = usbg_read_hex(path, name, "bDeviceProtocol", &buf);
	if (ret == USBG_SUCCESS)
		g_attrs->bDeviceProtocol = (uint8_t) buf;
	else
		goto out;

	ret = usbg_read_hex(path, name, "bMaxPacketSize0", &buf);
	if (ret == USBG_SUCCESS)
		g_attrs->bMaxPacketSize0 = (uint8_t) buf;
	else
		goto out;

	ret = usbg_read_hex(path, name, "idVendor", &buf);
	if (ret == USBG_SUCCESS)
		g_attrs->idVendor = (uint16_t) buf;
	else
		goto out;

	ret = usbg_read_hex(path, name, "idProduct", &buf);
	if (ret == USBG_SUCCESS)
		g_attrs->idProduct = (uint16_t) buf;
	else
		goto out;

out:
	return ret;
}

static usbg_gadget_strs *usbg_parse_strings(char *path, char *name, int lang,
		usbg_gadget_strs *g_strs)
{
	DIR *dir;
	char spath[USBG_MAX_PATH_LENGTH];

	sprintf(spath, "%s/%s/%s/0x%x", path, name, STRINGS_DIR, lang);

	/* Check if directory exist */
	dir = opendir(spath);
	if (dir) {
		closedir(dir);
		usbg_read_string(spath, "", "serialnumber", g_strs->str_ser);
		usbg_read_string(spath, "", "manufacturer", g_strs->str_mnf);
		usbg_read_string(spath, "", "product", g_strs->str_prd);
	} else {
		g_strs = NULL;
	}

	return g_strs;
}

static inline int usbg_parse_gadget(char *path, char *name, usbg_state *parent,
		usbg_gadget *g)
{
	int ret = USBG_SUCCESS;

	strcpy(g->name, name);
	strcpy(g->path, path);
	g->parent = parent;
	TAILQ_INIT(&g->functions);
	TAILQ_INIT(&g->configs);

	/* UDC bound to, if any */
	ret = usbg_read_string(path, g->name, "UDC", g->udc);
	if (ret != USBG_SUCCESS)
		goto out;

	ret = usbg_parse_functions(path, g);
	if (ret != USBG_SUCCESS)
		goto out;

	ret = usbg_parse_configs(path, g);
out:
	return ret;
}

static int usbg_parse_gadgets(char *path, usbg_state *s)
{
	usbg_gadget *g;
	int i, n;
	int ret = USBG_SUCCESS;
	struct dirent **dent;

	n = scandir(path, &dent, file_select, alphasort);
	if (n >= 0) {
		for (i = 0; i < n; i++) {
			/* Check if earlier gadgets
			 * has been created correctly */
			if (ret == USBG_SUCCESS) {
				/* Create new gadget and insert it into list */
				g = malloc(sizeof(usbg_gadget));
				if (g) {
					ret = usbg_parse_gadget(path, dent[i]->d_name, s, g);
					if (ret == USBG_SUCCESS)
						TAILQ_INSERT_TAIL(&s->gadgets, g, gnode);
					else
						usbg_free_gadget(g);
				} else {
					ret = USBG_ERROR_NO_MEM;
				}
			}
			free(dent[i]);
		}
		free(dent);
	} else {
		ret = usbg_translate_error(errno);
	}

	return ret;
}

static int usbg_init_state(char *path, usbg_state *s)
{
	int ret = USBG_SUCCESS;

	strcpy(s->path, path);
	TAILQ_INIT(&s->gadgets);

	ret = usbg_parse_gadgets(path, s);
	if (ret != USBG_SUCCESS)
		ERRORNO("unable to parse %s\n", path);

	return ret;
}

/*
 * User API
 */

int usbg_init(char *configfs_path, usbg_state **state)
{
	int ret = USBG_SUCCESS;
	DIR *dir;
	char path[USBG_MAX_PATH_LENGTH];

	sprintf(path, "%s/usb_gadget", configfs_path);

	/* Check if directory exist */
	dir = opendir(path);
	if (dir) {
		closedir(dir);
		*state = malloc(sizeof(usbg_state));
		ret = *state ? usbg_init_state(path, *state)
			 : USBG_ERROR_NO_MEM;
		if (*state && ret != USBG_SUCCESS) {
			ERRORNO("couldn't init gadget state\n");
			usbg_free_state(*state);
		}
	} else {
		ERRORNO("couldn't init gadget state\n");
		ret = usbg_translate_error(errno);
	}

	return ret;
}

void usbg_cleanup(usbg_state *s)
{
	usbg_free_state(s);
}

size_t usbg_get_configfs_path_len(usbg_state *s)
{
	return s ? strlen(s->path) : USBG_ERROR_INVALID_PARAM;
}

char *usbg_get_configfs_path(usbg_state *s, char *buf, size_t len)
{
	return s ? strncpy(buf, s->path, len) : NULL;
}

usbg_gadget *usbg_get_gadget(usbg_state *s, const char *name)
{
	usbg_gadget *g;

	TAILQ_FOREACH(g, &s->gadgets, gnode)
		if (!strcmp(g->name, name))
			return g;

	return NULL;
}

usbg_function *usbg_get_function(usbg_gadget *g, const char *name)
{
	usbg_function *f;

	TAILQ_FOREACH(f, &g->functions, fnode)
		if (!strcmp(f->name, name))
			return f;

	return NULL;
}

usbg_config *usbg_get_config(usbg_gadget *g, const char *name)
{
	usbg_config *c;

	TAILQ_FOREACH(c, &g->configs, cnode)
		if (!strcmp(c->name, name))
			return c;

	return NULL;
}

usbg_binding *usbg_get_binding(usbg_config *c, const char *name)
{
	usbg_binding *b;

	TAILQ_FOREACH(b, &c->bindings, bnode)
		if (!strcmp(b->name, name))
			return b;

	return NULL;
}

usbg_binding *usbg_get_link_binding(usbg_config *c, usbg_function *f)
{
	usbg_binding *b;

	TAILQ_FOREACH(b, &c->bindings, bnode)
		if (b->target == f)
			return b;

	return NULL;
}

static usbg_gadget *usbg_create_empty_gadget(usbg_state *s, char *name)
{
	char gpath[USBG_MAX_PATH_LENGTH];
	usbg_gadget *g;
	int ret;

	sprintf(gpath, "%s/%s", s->path, name);

	g = malloc(sizeof(usbg_gadget));
	if (!g) {
		ERRORNO("allocating gadget\n");
		return NULL;
	}

	TAILQ_INIT(&g->configs);
	TAILQ_INIT(&g->functions);
	strcpy(g->name, name);
	strcpy(g->path, s->path);
	g->parent = s;

	ret = mkdir(gpath, S_IRWXU|S_IRWXG|S_IRWXO);
	if (ret < 0) {
		ERRORNO("%s\n", gpath);
		free(g);
		return NULL;
	}

	/* Should be empty but read the default */
	usbg_read_string(g->path, g->name, "UDC", g->udc);

	return g;
}



usbg_gadget *usbg_create_gadget_vid_pid(usbg_state *s, char *name,
		uint16_t idVendor, uint16_t idProduct)
{
	usbg_gadget *g;

	if (!s)
		return NULL;

	g = usbg_get_gadget(s, name);
	if (g) {
		ERROR("duplicate gadget name\n");
		return NULL;
	}

	g = usbg_create_empty_gadget(s, name);

	/* Check if gadget creation was successful and set attributes */
	if (g) {
		usbg_write_hex16(s->path, name, "idVendor", idVendor);
		usbg_write_hex16(s->path, name, "idProduct", idProduct);

		INSERT_TAILQ_STRING_ORDER(&s->gadgets, ghead, name, g, gnode);
	}

	return g;
}

usbg_gadget *usbg_create_gadget(usbg_state *s, char *name,
		usbg_gadget_attrs *g_attrs, usbg_gadget_strs *g_strs)
{
	usbg_gadget *g;

	if (!s)
		return NULL;

	g = usbg_get_gadget(s, name);
	if (g) {
		ERROR("duplicate gadget name\n");
		return NULL;
	}

	g = usbg_create_empty_gadget(s, name);

	/* Check if gadget creation was successful and set attrs and strings */
	if (g) {
		if (g_attrs)
			usbg_set_gadget_attrs(g, g_attrs);

		if (g_strs)
			usbg_set_gadget_strs(g, LANG_US_ENG, g_strs);

		INSERT_TAILQ_STRING_ORDER(&s->gadgets, ghead, name, g, gnode);
	}

	return g;
}

usbg_gadget_attrs *usbg_get_gadget_attrs(usbg_gadget *g,
		usbg_gadget_attrs *g_attrs)
{
	if (g && g_attrs)
		usbg_parse_gadget_attrs(g->path, g->name, g_attrs);
	else
		g_attrs = NULL;

	return g_attrs;
}

size_t usbg_get_gadget_name_len(usbg_gadget *g)
{
	return g ? strlen(g->name) : USBG_ERROR_INVALID_PARAM;
}

char *usbg_get_gadget_name(usbg_gadget *g, char *buf, size_t len)
{
	return g ? strncpy(buf, g->name, len) : NULL;
}

size_t usbg_get_gadget_udc_len(usbg_gadget *g)
{
	return g ? strlen(g->udc) : USBG_ERROR_INVALID_PARAM;
}

char *usbg_get_gadget_udc(usbg_gadget *g, char *buf, size_t len)
{
	return g ? strncpy(buf, g->udc, len) : NULL;
}

void usbg_set_gadget_attrs(usbg_gadget *g, usbg_gadget_attrs *g_attrs)
{
	if (!g || !g_attrs)
		return;

	usbg_write_hex16(g->path, g->name, "bcdUSB", g_attrs->bcdUSB);
	usbg_write_hex8(g->path, g->name, "bDeviceClass", g_attrs->bDeviceClass);
	usbg_write_hex8(g->path, g->name, "bDeviceSubClass", g_attrs->bDeviceSubClass);
	usbg_write_hex8(g->path, g->name, "bDeviceProtocol", g_attrs->bDeviceProtocol);
	usbg_write_hex8(g->path, g->name, "bMaxPacketSize0", g_attrs->bMaxPacketSize0);
	usbg_write_hex16(g->path, g->name, "idVendor", g_attrs->idVendor);
	usbg_write_hex16(g->path, g->name, "idProduct", g_attrs->idProduct);
	usbg_write_hex16(g->path, g->name, "bcdDevice", g_attrs->bcdDevice);
}

void usbg_set_gadget_vendor_id(usbg_gadget *g, uint16_t idVendor)
{
	usbg_write_hex16(g->path, g->name, "idVendor", idVendor);
}

void usbg_set_gadget_product_id(usbg_gadget *g, uint16_t idProduct)
{
	usbg_write_hex16(g->path, g->name, "idProduct", idProduct);
}

void usbg_set_gadget_device_class(usbg_gadget *g, uint8_t bDeviceClass)
{
	usbg_write_hex8(g->path, g->name, "bDeviceClass", bDeviceClass);
}

void usbg_set_gadget_device_protocol(usbg_gadget *g, uint8_t bDeviceProtocol)
{
	usbg_write_hex8(g->path, g->name, "bDeviceProtocol", bDeviceProtocol);
}

void usbg_set_gadget_device_subclass(usbg_gadget *g, uint8_t bDeviceSubClass)
{
	usbg_write_hex8(g->path, g->name, "bDeviceSubClass", bDeviceSubClass);
}

void usbg_set_gadget_device_max_packet(usbg_gadget *g, uint8_t bMaxPacketSize0)
{
	usbg_write_hex8(g->path, g->name, "bMaxPacketSize0", bMaxPacketSize0);
}

void usbg_set_gadget_device_bcd_device(usbg_gadget *g, uint16_t bcdDevice)
{
	usbg_write_hex16(g->path, g->name, "bcdDevice", bcdDevice);
}

void usbg_set_gadget_device_bcd_usb(usbg_gadget *g, uint16_t bcdUSB)
{
	usbg_write_hex16(g->path, g->name, "bcdUSB", bcdUSB);
}

usbg_gadget_strs *usbg_get_gadget_strs(usbg_gadget *g, int lang,
		usbg_gadget_strs *g_strs)
{
	if (g && g_strs)
		g_strs = usbg_parse_strings(g->path, g->name, lang, g_strs);
	else
		g_strs = NULL;

	return g_strs;
}

void usbg_set_gadget_strs(usbg_gadget *g, int lang,
		usbg_gadget_strs *g_strs)
{
	char path[USBG_MAX_PATH_LENGTH];

	sprintf(path, "%s/%s/%s/0x%x", g->path, g->name, STRINGS_DIR, lang);

	mkdir(path, S_IRWXU|S_IRWXG|S_IRWXO);

	usbg_write_string(path, "", "serialnumber", g_strs->str_ser);
	usbg_write_string(path, "", "manufacturer", g_strs->str_mnf);
	usbg_write_string(path, "", "product", g_strs->str_prd);
}

void usbg_set_gadget_serial_number(usbg_gadget *g, int lang, char *serno)
{
	char path[USBG_MAX_PATH_LENGTH];

	sprintf(path, "%s/%s/%s/0x%x", g->path, g->name, STRINGS_DIR, lang);

	mkdir(path, S_IRWXU|S_IRWXG|S_IRWXO);

	usbg_write_string(path, "", "serialnumber", serno);
}

void usbg_set_gadget_manufacturer(usbg_gadget *g, int lang, char *mnf)
{
	char path[USBG_MAX_PATH_LENGTH];

	sprintf(path, "%s/%s/%s/0x%x", g->path, g->name, STRINGS_DIR, lang);

	mkdir(path, S_IRWXU|S_IRWXG|S_IRWXO);

	usbg_write_string(path, "", "manufacturer", mnf);
}

void usbg_set_gadget_product(usbg_gadget *g, int lang, char *prd)
{
	char path[USBG_MAX_PATH_LENGTH];

	sprintf(path, "%s/%s/%s/0x%x", g->path, g->name, STRINGS_DIR, lang);

	mkdir(path, S_IRWXU|S_IRWXG|S_IRWXO);

	usbg_write_string(path, "", "product", prd);
}

usbg_function *usbg_create_function(usbg_gadget *g, usbg_function_type type,
		char *instance, usbg_function_attrs *f_attrs)
{
	char fpath[USBG_MAX_PATH_LENGTH];
	char name[USBG_MAX_STR_LENGTH];
	usbg_function *f;
	int ret;

	if (!g)
		return NULL;

	/**
	 * @todo Check for legal function type
	 */
	sprintf(name, "%s.%s", function_names[type], instance);
	f = usbg_get_function(g, name);
	if (f) {
		ERROR("duplicate function name\n");
		return NULL;
	}

	sprintf(fpath, "%s/%s/%s/%s", g->path, g->name, FUNCTIONS_DIR, name);

	f = malloc(sizeof(usbg_function));
	if (!f) {
		ERRORNO("allocating function\n");
		return NULL;
	}

	strcpy(f->name, name);
	sprintf(f->path, "%s/%s/%s", g->path, g->name, FUNCTIONS_DIR);
	f->type = type;

	ret = mkdir(fpath, S_IRWXU|S_IRWXG|S_IRWXO);
	if (ret < 0) {
		ERRORNO("%s\n", fpath);
		free(f);
		return NULL;
	}

	if (f_attrs)
		usbg_set_function_attrs(f, f_attrs);

	INSERT_TAILQ_STRING_ORDER(&g->functions, fhead, name, f, fnode);

	return f;
}

usbg_config *usbg_create_config(usbg_gadget *g, char *name,
		usbg_config_attrs *c_attrs, usbg_config_strs *c_strs)
{
	char cpath[USBG_MAX_PATH_LENGTH];
	usbg_config *c;
	int ret;

	if (!g)
		return NULL;

	/**
	 * @todo Check for legal configuration name
	 */
	c = usbg_get_config(g, name);
	if (c) {
		ERROR("duplicate configuration name\n");
		return NULL;
	}

	sprintf(cpath, "%s/%s/%s/%s", g->path, g->name, CONFIGS_DIR, name);

	c = malloc(sizeof(usbg_config));
	if (!c) {
		ERRORNO("allocating configuration\n");
		return NULL;
	}

	TAILQ_INIT(&c->bindings);
	strcpy(c->name, name);
	sprintf(c->path, "%s/%s/%s", g->path, g->name, CONFIGS_DIR);

	ret = mkdir(cpath, S_IRWXU|S_IRWXG|S_IRWXO);
	if (ret < 0) {
		ERRORNO("%s\n", cpath);
		free(c);
		return NULL;
	}

	if (c_attrs)
		usbg_set_config_attrs(c, c_attrs);

	if (c_strs)
		usbg_set_config_string(c, LANG_US_ENG, c_strs->configuration);

	INSERT_TAILQ_STRING_ORDER(&g->configs, chead, name, c, cnode);

	return c;
}

size_t usbg_get_config_name_len(usbg_config *c)
{
	return c ? strlen(c->name) : USBG_ERROR_INVALID_PARAM;
}

char *usbg_get_config_name(usbg_config *c, char *buf, size_t len)
{
	return c ? strncpy(buf, c->name, len) : NULL;
}

size_t usbg_get_function_name_len(usbg_function *f)
{
	return f ? strlen(f->name) : USBG_ERROR_INVALID_PARAM;
}

char *usbg_get_function_name(usbg_function *f, char *buf, size_t len)
{
	return f ? strncpy(buf, f->name, len) : NULL;
}

void usbg_set_config_attrs(usbg_config *c, usbg_config_attrs *c_attrs)
{
	if (!c || !c_attrs)
		return;

	usbg_write_dec(c->path, c->name, "MaxPower", c_attrs->bMaxPower);
	usbg_write_hex8(c->path, c->name, "bmAttributes", c_attrs->bmAttributes);
}

usbg_config_attrs *usbg_get_config_attrs(usbg_config *c,
		usbg_config_attrs *c_attrs)
{
	if (c && c_attrs)
		usbg_parse_config_attrs(c->path, c->name, c_attrs);
	else
		c_attrs = NULL;

	return c_attrs;
}

void usbg_set_config_max_power(usbg_config *c, int bMaxPower)
{
	usbg_write_dec(c->path, c->name, "MaxPower", bMaxPower);
}

void usbg_set_config_bm_attrs(usbg_config *c, int bmAttributes)
{
	usbg_write_hex8(c->path, c->name, "bmAttributes", bmAttributes);
}

usbg_config_strs *usbg_get_config_strs(usbg_config *c, int lang,
		usbg_config_strs *c_strs)
{
	if (c && c_strs)
		c_strs = usbg_parse_config_strs(c->path, c->name, lang, c_strs);
	else
		c_strs = NULL;

	return c_strs;
}

void usbg_set_config_strs(usbg_config *c, int lang,
		usbg_config_strs *c_strs)
{
	usbg_set_config_string(c, lang, c_strs->configuration);
}

void usbg_set_config_string(usbg_config *c, int lang, char *str)
{
	char path[USBG_MAX_PATH_LENGTH];

	sprintf(path, "%s/%s/%s/0x%x", c->path, c->name, STRINGS_DIR, lang);

	mkdir(path, S_IRWXU|S_IRWXG|S_IRWXO);

	usbg_write_string(path, "", "configuration", str);
}

int usbg_add_config_function(usbg_config *c, char *name, usbg_function *f)
{
	char bpath[USBG_MAX_PATH_LENGTH];
	char fpath[USBG_MAX_PATH_LENGTH];
	usbg_binding *b;
	int ret = -1;

	if (!c || !f)
		return ret;

	b = usbg_get_binding(c, name);
	if (b) {
		ERROR("duplicate binding name\n");
		return ret;
	}

	b = usbg_get_link_binding(c, f);
	if (b) {
		ERROR("duplicate binding link\n");
		return ret;
	}

	sprintf(bpath, "%s/%s/%s", c->path, c->name, name);
	sprintf(fpath, "%s/%s", f->path, f->name);

	b = malloc(sizeof(usbg_binding));
	if (!b) {
		ERRORNO("allocating binding\n");
		return -1;
	}

	ret = symlink(fpath, bpath);
	if (ret < 0) {
		ERRORNO("%s -> %s\n", bpath, fpath);
		return ret;
	}

	strcpy(b->name, name);
	strcpy(b->path, bpath);
	b->target = f;
	b->parent = c;

	INSERT_TAILQ_STRING_ORDER(&c->bindings, bhead, name, b, bnode);

	return 0;
}

usbg_function *usbg_get_binding_target(usbg_binding *b)
{
	return b ? b->target : NULL;
}

size_t usbg_get_binding_name_len(usbg_binding *b)
{
	return b ? strlen(b->name) : USBG_ERROR_INVALID_PARAM;
}

char *usbg_get_binding_name(usbg_binding *b, char *buf, size_t len)
{
	return b ? strncpy(buf, b->name, len) : NULL;
}

int usbg_get_udcs(struct dirent ***udc_list)
{
	return scandir("/sys/class/udc", udc_list, file_select, alphasort);
}

void usbg_enable_gadget(usbg_gadget *g, char *udc)
{
	char gudc[USBG_MAX_STR_LENGTH];
	struct dirent **udc_list;
	int n;

	if (!udc) {
		n = usbg_get_udcs(&udc_list);
		if (!n)
			return;
		strcpy(gudc, udc_list[0]->d_name);
		while (n--)
			free(udc_list[n]);
		free(udc_list);
	} else
		strcpy (gudc, udc);

	strcpy(g->udc, gudc);
	usbg_write_string(g->path, g->name, "UDC", gudc);
}

void usbg_disable_gadget(usbg_gadget *g)
{
	strcpy(g->udc, "");
	usbg_write_string(g->path, g->name, "UDC", "");
}

/*
 * USB function-specific attribute configuration
 */

usbg_function_type usbg_get_function_type(usbg_function *f)
{
	return f->type;
}

usbg_function_attrs *usbg_get_function_attrs(usbg_function *f,
		usbg_function_attrs *f_attrs)
{
	if (f && f_attrs)
		usbg_parse_function_attrs(f, f_attrs);
	else
		f_attrs = NULL;

	return f_attrs;
}

void usbg_set_function_attrs(usbg_function *f, usbg_function_attrs *f_attrs)
{
	char *addr;

	if (!f || !f_attrs)
		return;

	switch (f->type) {
	case F_SERIAL:
	case F_ACM:
	case F_OBEX:
		usbg_write_dec(f->path, f->name, "port_num", f_attrs->serial.port_num);
		break;
	case F_ECM:
	case F_SUBSET:
	case F_NCM:
	case F_EEM:
	case F_RNDIS:
		addr = ether_ntoa(&f_attrs->net.dev_addr);
		usbg_write_string(f->path, f->name, "dev_addr", addr);

		addr = ether_ntoa(&f_attrs->net.host_addr);
		usbg_write_string(f->path, f->name, "host_addr", addr);

		usbg_write_string(f->path, f->name, "ifname", f_attrs->net.ifname);

		usbg_write_dec(f->path, f->name, "qmult", f_attrs->net.qmult);
		break;
	case F_PHONET:
		usbg_write_string(f->path, f->name, "ifname", f_attrs->phonet.ifname);
		break;
	default:
		ERROR("Unsupported function type\n");
	}
}

void usbg_set_net_dev_addr(usbg_function *f, struct ether_addr *dev_addr)
{
	char *str_addr;

	str_addr = ether_ntoa(dev_addr);
	usbg_write_string(f->path, f->name, "dev_addr", str_addr);
}

void usbg_set_net_host_addr(usbg_function *f, struct ether_addr *host_addr)
{
	char *str_addr;

	str_addr = ether_ntoa(host_addr);
	usbg_write_string(f->path, f->name, "host_addr", str_addr);
}

void usbg_set_net_qmult(usbg_function *f, int qmult)
{
	usbg_write_dec(f->path, f->name, "qmult", qmult);
}

usbg_gadget *usbg_get_first_gadget(usbg_state *s)
{
	return s ? TAILQ_FIRST(&s->gadgets) : NULL;
}

usbg_function *usbg_get_first_function(usbg_gadget *g)
{
	return g ? TAILQ_FIRST(&g->functions) : NULL;
}

usbg_config *usbg_get_first_config(usbg_gadget *g)
{
	return g ? TAILQ_FIRST(&g->configs) : NULL;
}

usbg_binding *usbg_get_first_binding(usbg_config *c)
{
	return c ? TAILQ_FIRST(&c->bindings) : NULL;
}

usbg_gadget *usbg_get_next_gadget(usbg_gadget *g)
{
	return g ? TAILQ_NEXT(g, gnode) : NULL;
}

usbg_function *usbg_get_next_function(usbg_function *f)
{
	return f ? TAILQ_NEXT(f, fnode) : NULL;
}

usbg_config *usbg_get_next_config(usbg_config *c)
{
	return c ? TAILQ_NEXT(c, cnode) : NULL;
}

usbg_binding *usbg_get_next_binding(usbg_binding *b)
{
	return b ? TAILQ_NEXT(b, bnode) : NULL;
}
