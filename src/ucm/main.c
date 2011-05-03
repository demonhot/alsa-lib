/*
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software  
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 *  Support for the verb/device/modifier core logic and API,
 *  command line tool and file parser was kindly sponsored by
 *  Texas Instruments Inc.
 *  Support for multiple active modifiers and devices,
 *  transition sequences, multiple client access and user defined use
 *  cases was kindly sponsored by Wolfson Microelectronics PLC.
 *
 *  Copyright (C) 2008-2010 SlimLogic Ltd
 *  Copyright (C) 2010 Wolfson Microelectronics PLC
 *  Copyright (C) 2010 Texas Instruments Inc.
 *  Copyright (C) 2010 Red Hat Inc.
 *  Authors: Liam Girdwood <lrg@slimlogic.co.uk>
 *	         Stefan Schmidt <stefan@slimlogic.co.uk>
 *	         Justin Xu <justinx@slimlogic.co.uk>
 *               Jaroslav Kysela <perex@perex.cz>
 */

#include "ucm_local.h"
#include <stdarg.h>
#include <pthread.h>

/*
 * misc
 */

static int get_value1(const char **value, struct list_head *value_list,
                      const char *identifier);
static int get_value3(const char **value,
		      const char *identifier,
		      struct list_head *value_list1,
		      struct list_head *value_list2,
		      struct list_head *value_list3);

static int check_identifier(const char *identifier, const char *prefix)
{
	int len;

	if (strcmp(identifier, prefix) == 0)
		return 1;
	len = strlen(prefix);
	if (memcmp(identifier, prefix, len) == 0 && identifier[len] == '/')
		return 1;
	return 0;
}

static int list_count(struct list_head *list)
{
        struct list_head *pos;
        int count = 0;
        
        list_for_each(pos, list) {
                count += 1;
        }
        return count;
}

static int alloc_str_list(struct list_head *list, int mult, char **result[])
{
        char **res;
        int cnt;
        
        cnt = list_count(list) * mult;
        if (cnt == 0) {
		*result = NULL;
                return cnt;
	}
        res = calloc(mult, cnt * sizeof(char *));
        if (res == NULL)
                return -ENOMEM;
        *result = res;
        return cnt;
}

/**
 * \brief Create an identifier
 * \param fmt Format (sprintf like)
 * \param ... Optional arguments for sprintf like format
 * \return Allocated string identifier or NULL on error
 */
char *snd_use_case_identifier(const char *fmt, ...)
{
	char *str, *res;
	int size = strlen(fmt) + 512;
	va_list args;

	str = malloc(size);
	if (str == NULL)
		return NULL;
	va_start(args, fmt);
	vsnprintf(str, size, fmt, args);
	va_end(args);
	str[size-1] = '\0';
	res = realloc(str, strlen(str) + 1);
	if (res)
		return res;
	return str;
}

/**
 * \brief Free a string list
 * \param list The string list to free
 * \param items Count of strings
 * \return Zero if success, otherwise a negative error code
 */
int snd_use_case_free_list(const char *list[], int items)
{
        int i;
	if (list == NULL)
		return 0;
        for (i = 0; i < items; i++)
		free((void *)list[i]);
        free(list);
	return 0;
}

static int open_ctl(snd_use_case_mgr_t *uc_mgr,
		    snd_ctl_t **ctl,
		    const char *ctl_dev)
{
	int err;

	/* FIXME: add a list of ctl devices to uc_mgr structure and
           cache accesses for multiple opened ctl devices */
	if (uc_mgr->ctl_dev != NULL && strcmp(ctl_dev, uc_mgr->ctl_dev) == 0) {
		*ctl = uc_mgr->ctl;
		return 0;
	}
	if (uc_mgr->ctl_dev) {
		free(uc_mgr->ctl_dev);
		uc_mgr->ctl_dev = NULL;
		snd_ctl_close(uc_mgr->ctl);
	
	}
	err = snd_ctl_open(ctl, ctl_dev, 0);
	if (err < 0)
		return err;
	uc_mgr->ctl_dev = strdup(ctl_dev);
	if (uc_mgr->ctl_dev == NULL) {
		snd_ctl_close(*ctl);
		return -ENOMEM;
	}
	uc_mgr->ctl = *ctl;
	return 0;
}

static int execute_cset(snd_ctl_t *ctl, char *cset)
{
	char *pos;
	int err;
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_value_t *value;
	snd_ctl_elem_info_t *info;

	snd_ctl_elem_id_malloc(&id);
	snd_ctl_elem_value_malloc(&value);
	snd_ctl_elem_info_malloc(&info);

	pos = strrchr(cset, ' ');
	if (pos == NULL) {
		uc_error("undefined value for cset >%s<", cset);
		err =  -EINVAL;
		goto __fail;
	}
	*pos = '\0';
	err = snd_ctl_ascii_elem_id_parse(id, cset);
	if (err < 0)
		goto __fail;
	snd_ctl_elem_value_set_id(value, id);
	snd_ctl_elem_info_set_id(info, id);
	err = snd_ctl_elem_read(ctl, value);
	if (err < 0)
		goto __fail;
	err = snd_ctl_elem_info(ctl, info);
	if (err < 0)
		goto __fail;
	err = snd_ctl_ascii_value_parse(ctl, value, info, pos + 1);
	if (err < 0)
		goto __fail;
	err = snd_ctl_elem_write(ctl, value);
	if (err < 0)
		goto __fail;
	err = 0;
      __fail:
	*pos = ' ';

	if (id != NULL)
		free(id);
	if (value != NULL)
		free(value);
	if (info != NULL)
		free(info);

	return err;
}

/**
 * \brief Execute the sequence
 * \param uc_mgr Use case manager
 * \param seq Sequence
 * \return zero on success, otherwise a negative error code
 */
static int execute_sequence(snd_use_case_mgr_t *uc_mgr,
			    struct list_head *seq,
			    struct list_head *value_list1,
			    struct list_head *value_list2,
			    struct list_head *value_list3)
{
	struct list_head *pos;
	struct sequence_element *s;
	char *cdev = NULL;
	snd_ctl_t *ctl = NULL;
	int err = 0;

	list_for_each(pos, seq) {
		s = list_entry(pos, struct sequence_element, list);
		switch (s->type) {
		case SEQUENCE_ELEMENT_TYPE_CDEV:
			cdev = strdup(s->data.cdev);
			if (cdev == NULL)
				goto __fail_nomem;
			break;
		case SEQUENCE_ELEMENT_TYPE_CSET:
			if (cdev == NULL) {
				const char *cdev1 = NULL, *cdev2 = NULL;
				err = get_value3(&cdev1, "PlaybackCTL",
						 value_list1,
						 value_list2,
						 value_list3);
				if (err < 0 && err != ENOENT) {
					uc_error("cdev is not defined!");
					return err;
				}
				err = get_value3(&cdev1, "CaptureCTL",
						 value_list1,
						 value_list2,
						 value_list3);
				if (err < 0 && err != ENOENT) {
					free((char *)cdev1);
					uc_error("cdev is not defined!");
					return err;
				}
				if (cdev1 == NULL || cdev2 == NULL ||
                                    strcmp(cdev1, cdev2) == 0) {
					cdev = (char *)cdev1;
					free((char *)cdev2);
				} else {
					free((char *)cdev1);
					free((char *)cdev2);
				}
			}
			if (ctl == NULL) {
				err = open_ctl(uc_mgr, &ctl, cdev);
				if (err < 0) {
					uc_error("unable to open ctl device '%s'", cdev);
					goto __fail;
				}
			}
			err = execute_cset(ctl, s->data.cset);
			if (err < 0) {
				uc_error("unable to execute cset '%s'\n", s->data.cset);
				goto __fail;
			}
			break;
		case SEQUENCE_ELEMENT_TYPE_SLEEP:
			usleep(s->data.sleep);
			break;
		case SEQUENCE_ELEMENT_TYPE_EXEC:
			err = system(s->data.exec);
			if (err < 0)
				goto __fail;
			break;
		default:
			uc_error("unknown sequence command %i", s->type);
			break;
		}
	}
	free(cdev);
	return 0;
      __fail_nomem:
	err = -ENOMEM;
      __fail:
	free(cdev);
	return err;

}

/**
 * \brief Import master config and execute the default sequence
 * \param uc_mgr Use case manager
 * \return zero on success, otherwise a negative error code
 */
static int import_master_config(snd_use_case_mgr_t *uc_mgr)
{
	int err;
	
	err = uc_mgr_import_master_config(uc_mgr);
	if (err < 0)
		return err;
	err = execute_sequence(uc_mgr, &uc_mgr->default_list,
			       &uc_mgr->value_list, NULL, NULL);
	if (err < 0)
		uc_error("Unable to execute default sequence");
	return err;
}

/**
 * \brief Universal find - string in a list
 * \param list List of structures
 * \param offset Offset of list structure
 * \param soffset Offset of string structure
 * \param match String to match
 * \return structure on success, otherwise a NULL (not found)
 */
static void *find0(struct list_head *list,
		   unsigned long offset,
		   unsigned long soffset,
		   const char *match)
{
	struct list_head *pos;
	char *ptr, *str;

	list_for_each(pos, list) {
		ptr = list_entry_offset(pos, char, offset);
		str = *((char **)(ptr + soffset));
		if (strcmp(str, match) == 0)
			return ptr;
	}
	return NULL;
}

#define find(list, type, member, value, match) \
	find0(list, (unsigned long)(&((type *)0)->member), \
		    (unsigned long)(&((type *)0)->value), match)

/**
 * \brief Universal string list
 * \param list List of structures
 * \param result Result list
 * \param offset Offset of list structure
 * \param s1offset Offset of string structure
 * \return count of items on success, otherwise a negative error code
 */
static int get_list0(struct list_head *list,
                     const char **result[],
                     unsigned long offset,
                     unsigned long s1offset)
{
        char **res;
        int cnt;
	struct list_head *pos;
	char *ptr, *str1;

	cnt = alloc_str_list(list, 1, &res);
	if (cnt <= 0)
	        return cnt;
	*result = (const char **)res;
	list_for_each(pos, list) {
		ptr = list_entry_offset(pos, char, offset);
		str1 = *((char **)(ptr + s1offset));
		if (str1 != NULL) {
		        *res = strdup(str1);
		        if (*res == NULL)
		                goto __fail;
                } else {
                        *res = NULL;
                }
                res++;
	}
	return cnt;
      __fail:
        snd_use_case_free_list((const char **)res, cnt);
        return -ENOMEM;
}

#define get_list(list, result, type, member, s1) \
	get_list0(list, result, \
	            (unsigned long)(&((type *)0)->member), \
		    (unsigned long)(&((type *)0)->s1))

/**
 * \brief Universal string list - pair of strings
 * \param list List of structures
 * \param result Result list
 * \param offset Offset of list structure
 * \param s1offset Offset of string structure
 * \param s1offset Offset of string structure
 * \return count of items on success, otherwise a negative error code
 */
static int get_list20(struct list_head *list,
                      const char **result[],
                      unsigned long offset,
                      unsigned long s1offset,
                      unsigned long s2offset)
{
        char **res;
        int cnt;
	struct list_head *pos;
	char *ptr, *str1, *str2;

	cnt = alloc_str_list(list, 2, &res);
	if (cnt <= 0)
	        return cnt;
        *result = (const char **)res;
	list_for_each(pos, list) {
		ptr = list_entry_offset(pos, char, offset);
		str1 = *((char **)(ptr + s1offset));
		if (str1 != NULL) {
		        *res = strdup(str1);
		        if (*res == NULL)
		                goto __fail;
                } else {
                        *res = NULL;
                }
                res++;
		str2 = *((char **)(ptr + s2offset));
		if (str2 != NULL) {
		        *res = strdup(str2);
		        if (*res == NULL)
		                goto __fail;
                } else {
                        *res = NULL;
                }
                res++;
	}
	return cnt;
      __fail:
        snd_use_case_free_list((const char **)res, cnt);
        return -ENOMEM;
}

#define get_list2(list, result, type, member, s1, s2) \
	get_list20(list, result, \
	            (unsigned long)(&((type *)0)->member), \
		    (unsigned long)(&((type *)0)->s1), \
		    (unsigned long)(&((type *)0)->s2))

/**
 * \brief Find verb
 * \param uc_mgr Use case manager
 * \param verb_name verb to find
 * \return structure on success, otherwise a NULL (not found)
 */
static inline struct use_case_verb *find_verb(snd_use_case_mgr_t *uc_mgr,
					      const char *verb_name)
{
	return find(&uc_mgr->verb_list,
		    struct use_case_verb, list, name,
		    verb_name);
}

/**
 * \brief Find device
 * \param verb Use case verb
 * \param device_name device to find
 * \return structure on success, otherwise a NULL (not found)
 */
static inline struct use_case_device *
        find_device(struct use_case_verb *verb,
                      const char *device_name)
{
	return find(&verb->device_list,
		    struct use_case_device, list, name,
		    device_name);
}

static int is_modifier_supported(snd_use_case_mgr_t *uc_mgr, 
	struct use_case_modifier *modifier)
{
	struct dev_list *device;
	struct use_case_device *adev;
	struct list_head *pos, *pos1;
	char *cpos;
	int dlen, len;

	list_for_each(pos, &modifier->dev_list) {
		device = list_entry(pos, struct dev_list, list);
		cpos = strchr(device->name, '.');
		if (cpos) {
			if (find(&uc_mgr->active_devices,
					struct use_case_device, active_list,
					name, device->name))
				return 1;
		} else {
			dlen = strlen(device->name);
			list_for_each(pos1, &uc_mgr->active_devices) {
				adev = list_entry(pos1, struct use_case_device,
						  active_list);
				cpos = strchr(adev->name, '.');
				if (cpos)
					len = cpos - adev->name;
				else
					len = strlen(adev->name);
				if (len != dlen)
					continue;
				if (memcmp(adev->name, device->name, len))
					continue;
				return 1;
			}
		}
	}
	return 0;
}

/**
 * \brief Find modifier
 * \param verb Use case verb
 * \param modifier_name modifier to find
 * \return structure on success, otherwise a NULL (not found)
 */
static struct use_case_modifier *
        find_modifier(snd_use_case_mgr_t *uc_mgr, const char *modifier_name)
{
	struct use_case_modifier *modifier;
	struct use_case_verb *verb = uc_mgr->active_verb;
	struct list_head *pos;
	char name[64], *cpos;

	list_for_each(pos, &verb->modifier_list) {
		modifier = list_entry(pos, struct use_case_modifier, list);

		strncpy(name, modifier->name, sizeof(name));
		name[sizeof(name)-1] = '\0';
		cpos = strchr(name, '.');
		if (!cpos)
			continue;
		*cpos= '\0';

		if (strcmp(name, modifier_name))
			continue;

		if (is_modifier_supported(uc_mgr, modifier))
			return modifier;
	}
	return NULL;
}

/**
 * \brief Set verb
 * \param uc_mgr Use case manager
 * \param verb verb to set
 * \param enable nonzero = enable, zero = disable
 * \return zero on success, otherwise a negative error code
 */
static int set_verb(snd_use_case_mgr_t *uc_mgr,
		    struct use_case_verb *verb,
		    int enable)
{
	struct list_head *seq;
	int err;

	if (enable) {
		seq = &verb->enable_list;
	} else {
		seq = &verb->disable_list;
	}
	err = execute_sequence(uc_mgr, seq,
			       &verb->value_list,
			       &uc_mgr->value_list,
			       NULL);
	if (enable && err >= 0)
		uc_mgr->active_verb = verb;
	return err;
}

/**
 * \brief Set modifier
 * \param uc_mgr Use case manager
 * \param modifier modifier to set
 * \param enable nonzero = enable, zero = disable
 * \return zero on success, otherwise a negative error code
 */
static int set_modifier(snd_use_case_mgr_t *uc_mgr,
			struct use_case_modifier *modifier,
			int enable)
{
	struct list_head *seq;
	int err;

	if (enable) {
		seq = &modifier->enable_list;
	} else {
		seq = &modifier->disable_list;
	}
	err = execute_sequence(uc_mgr, seq,
			       &modifier->value_list,
			       &uc_mgr->active_verb->value_list,
			       &uc_mgr->value_list);
	if (enable && err >= 0) {
		list_add_tail(&modifier->active_list, &uc_mgr->active_modifiers);
	} else if (!enable) {
		list_del(&modifier->active_list);
	}
	return err;
}

/**
 * \brief Set device
 * \param uc_mgr Use case manager
 * \param device device to set
 * \param enable nonzero = enable, zero = disable
 * \return zero on success, otherwise a negative error code
 */
static int set_device(snd_use_case_mgr_t *uc_mgr,
		      struct use_case_device *device,
		      int enable)
{
	struct list_head *seq;
	int err;

	if (enable) {
		seq = &device->enable_list;
	} else {
		seq = &device->disable_list;
	}
	err = execute_sequence(uc_mgr, seq,
			       &device->value_list,
			       &uc_mgr->active_verb->value_list,
			       &uc_mgr->value_list);
	if (enable && err >= 0) {
		list_add_tail(&device->active_list, &uc_mgr->active_devices);
	} else if (!enable) {
		list_del(&device->active_list);
	}
	return err;
}

/**
 * \brief Init sound card use case manager.
 * \param uc_mgr Returned use case manager pointer
 * \param card_name name of card to open
 * \return zero on success, otherwise a negative error code
 */
int snd_use_case_mgr_open(snd_use_case_mgr_t **mgr,
			  const char *card_name)
{
	snd_use_case_mgr_t *uc_mgr;
	int err;

	/* create a new UCM */
	uc_mgr = calloc(1, sizeof(snd_use_case_mgr_t));
	if (uc_mgr == NULL)
		return -ENOMEM;
	INIT_LIST_HEAD(&uc_mgr->verb_list);
	INIT_LIST_HEAD(&uc_mgr->default_list);
	INIT_LIST_HEAD(&uc_mgr->value_list);
	INIT_LIST_HEAD(&uc_mgr->active_modifiers);
	INIT_LIST_HEAD(&uc_mgr->active_devices);
	pthread_mutex_init(&uc_mgr->mutex, NULL);

	uc_mgr->card_name = strdup(card_name);
	if (uc_mgr->card_name == NULL) {
		free(uc_mgr);
		return -ENOMEM;
	}

	/* get info on use_cases and verify against card */
	err = import_master_config(uc_mgr);
	if (err < 0) {
		uc_error("error: failed to import %s use case configuration %d",
			card_name, err);
		goto err;
	}

	*mgr = uc_mgr;
	return 0;

err:
	uc_mgr_free(uc_mgr);
	return err;
}

/**
 * \brief Reload and reparse all use case files.
 * \param uc_mgr Use case manager
 * \return zero on success, otherwise a negative error code
 */
int snd_use_case_mgr_reload(snd_use_case_mgr_t *uc_mgr)
{
	int err;

	pthread_mutex_lock(&uc_mgr->mutex);

	uc_mgr_free_verb(uc_mgr);

	/* reload all use cases */
	err = import_master_config(uc_mgr);
	if (err < 0) {
		uc_error("error: failed to reload use cases\n");
		pthread_mutex_unlock(&uc_mgr->mutex);
		return -EINVAL;
	}

	pthread_mutex_unlock(&uc_mgr->mutex);
	return err;
}

/**
 * \brief Close use case manager.
 * \param uc_mgr Use case manager
 * \return zero on success, otherwise a negative error code
 */
int snd_use_case_mgr_close(snd_use_case_mgr_t *uc_mgr)
{
	uc_mgr_free(uc_mgr);

	return 0;
}

/*
 * Tear down current use case verb, device and modifier.
 */
static int dismantle_use_case(snd_use_case_mgr_t *uc_mgr)
{
	struct list_head *pos, *npos;
	struct use_case_modifier *modifier;
	struct use_case_device *device;
	int err;

	list_for_each_safe(pos, npos, &uc_mgr->active_modifiers) {
		modifier = list_entry(pos, struct use_case_modifier,
				      active_list);
		err = set_modifier(uc_mgr, modifier, 0);
		if (err < 0)
			uc_error("Unable to disable modifier %s", modifier->name);
	}
	INIT_LIST_HEAD(&uc_mgr->active_modifiers);

	list_for_each_safe(pos, npos, &uc_mgr->active_devices) {
		device = list_entry(pos, struct use_case_device,
				    active_list);
		err = set_device(uc_mgr, device, 0);
		if (err < 0)
			uc_error("Unable to disable device %s", device->name);
	}
	INIT_LIST_HEAD(&uc_mgr->active_devices);

	err = set_verb(uc_mgr, uc_mgr->active_verb, 0);
	if (err < 0) {
		uc_error("Unable to disable verb %s", uc_mgr->active_verb->name);
		return err;
	}
	uc_mgr->active_verb = NULL;

	err = execute_sequence(uc_mgr, &uc_mgr->default_list,
			       &uc_mgr->value_list, NULL, NULL);
	
	return err;
}

/**
 * \brief Reset sound card controls to default values.
 * \param uc_mgr Use case manager
 * \return zero on success, otherwise a negative error code
 */
int snd_use_case_mgr_reset(snd_use_case_mgr_t *uc_mgr)
{
        int err;

	pthread_mutex_lock(&uc_mgr->mutex);
	err = execute_sequence(uc_mgr, &uc_mgr->default_list,
			       &uc_mgr->value_list, NULL, NULL);
	INIT_LIST_HEAD(&uc_mgr->active_modifiers);
	INIT_LIST_HEAD(&uc_mgr->active_devices);
	uc_mgr->active_verb = NULL;
	pthread_mutex_unlock(&uc_mgr->mutex);
	return err;
}

/**
 * \brief Get list of verbs in pair verbname+comment
 * \param list Returned list
 * \param verbname For verb (NULL = current)
 * \return Number of list entries if success, otherwise a negative error code
 */
static int get_verb_list(snd_use_case_mgr_t *uc_mgr, const char **list[])
{
        return get_list2(&uc_mgr->verb_list, list,
                         struct use_case_verb, list,
                         name, comment);
}

/**
 * \brief Get list of devices in pair devicename+comment
 * \param list Returned list
 * \param verbname For verb (NULL = current)
 * \return Number of list entries if success, otherwise a negative error code
 */
static int get_device_list(snd_use_case_mgr_t *uc_mgr, const char **list[],
                           char *verbname)
{
        struct use_case_verb *verb;
        
        if (verbname) {
                verb = find_verb(uc_mgr, verbname);
        } else {
                verb = uc_mgr->active_verb;
        }
        if (verb == NULL)
                return -ENOENT;
        return get_list2(&verb->device_list, list,
                         struct use_case_device, list,
                         name, comment);
}

/**
 * \brief Get list of modifiers in pair devicename+comment
 * \param list Returned list
 * \param verbname For verb (NULL = current)
 * \return Number of list entries if success, otherwise a negative error code
 */
static int get_modifier_list(snd_use_case_mgr_t *uc_mgr, const char **list[],
                             char *verbname)
{
        struct use_case_verb *verb;
        
        if (verbname) {
                verb = find_verb(uc_mgr, verbname);
        } else {
                verb = uc_mgr->active_verb;
        }
        if (verb == NULL)
                return -ENOENT;
        return get_list2(&verb->modifier_list, list,
                         struct use_case_modifier, list,
                         name, comment);
}

struct myvalue {
        struct list_head list;
        char *value;
};

static int add_values(struct list_head *list,
                      const char *identifier,
                      struct list_head *source)
{
        struct ucm_value *v;
        struct myvalue *val;
        struct list_head *pos, *pos1;
        int match;
        
        list_for_each(pos, source) {
                v = list_entry(pos, struct ucm_value, list);
                if (check_identifier(identifier, v->name)) {
                        match = 0;
                        list_for_each(pos1, list) {
                                val = list_entry(pos1, struct myvalue, list);
                                if (strcmp(val->value, v->data) == 0) {
                                        match = 1;
                                        break;
                                }
                        }
                        if (!match) {
                                val = malloc(sizeof(struct myvalue));
                                if (val == NULL)
                                        return -ENOMEM;
				val->value = v->data;
                                list_add_tail(&val->list, list);
                        }
                }
        }
        return 0;
}

/**
 * \brief Get list of values
 * \param list Returned list
 * \param verbname For verb (NULL = current)
 * \return Number of list entries if success, otherwise a negative error code
 */
static int get_value_list(snd_use_case_mgr_t *uc_mgr,
                          const char *identifier,
                          const char **list[],
                          char *verbname)
{
        struct list_head mylist, *pos, *npos;
        struct myvalue *val;
        struct use_case_verb *verb;
        struct use_case_device *dev;
        struct use_case_modifier *mod;
        char **res;
        int err;
        
        if (verbname) {
                verb = find_verb(uc_mgr, verbname);
        } else {
                verb = uc_mgr->active_verb;
        }
        if (verb == NULL)
                return -ENOENT;
        INIT_LIST_HEAD(&mylist);
	err = add_values(&mylist, identifier, &uc_mgr->value_list);
	if (err < 0)
		goto __fail;
        err = add_values(&mylist, identifier, &verb->value_list);
        if (err < 0)
                goto __fail;
        list_for_each(pos, &verb->device_list) {
                dev = list_entry(pos, struct use_case_device, list);
                err = add_values(&mylist, identifier, &dev->value_list);
                if (err < 0)
                        goto __fail;
        }
        list_for_each(pos, &verb->modifier_list) {
                mod = list_entry(pos, struct use_case_modifier, list);
                err = add_values(&mylist, identifier, &mod->value_list);
                if (err < 0)
                        goto __fail;
        }
        err = alloc_str_list(&mylist, 1, &res);
        if (err >= 0) {
	        *list = (const char **)res;
                list_for_each(pos, &mylist) {
                        val = list_entry(pos, struct myvalue, list);
                        *res = strdup(val->value);
                        if (*res == NULL) {
                                snd_use_case_free_list((const char **)res, err);
                                err = -ENOMEM;
                                goto __fail;
                        }
                        res++;
                }
        }
      __fail:
        list_for_each_safe(pos, npos, &mylist) {
                val = list_entry(pos, struct myvalue, list);
                list_del(&val->list);
                free(val);
        }
        return err;
}

/**
 * \brief Get list of enabled devices
 * \param list Returned list
 * \param verbname For verb (NULL = current)
 * \return Number of list entries if success, otherwise a negative error code
 */
static int get_enabled_device_list(snd_use_case_mgr_t *uc_mgr,
                                   const char **list[])
{
        if (uc_mgr->active_verb == NULL)
                return -EINVAL;
        return get_list(&uc_mgr->active_devices, list,
                        struct use_case_device, active_list,
                        name);
}

/**
 * \brief Get list of enabled modifiers
 * \param list Returned list
 * \param verbname For verb (NULL = current)
 * \return Number of list entries if success, otherwise a negative error code
 */
static int get_enabled_modifier_list(snd_use_case_mgr_t *uc_mgr,
                                     const char **list[])
{
        if (uc_mgr->active_verb == NULL)
                return -EINVAL;
        return get_list(&uc_mgr->active_modifiers, list,
                        struct use_case_modifier, active_list,
                        name);
}

/**
 * \brief Obtain a list of entries
 * \param uc_mgr Use case manager (may be NULL - card list)
 * \param identifier (may be NULL - card list)
 * \param list Returned allocated list
 * \return Number of list entries if success, otherwise a negative error code
 */
int snd_use_case_get_list(snd_use_case_mgr_t *uc_mgr,
			  const char *identifier,
			  const char **list[])
{
	char *str, *str1;
	int err;

	if (uc_mgr == NULL || identifier == NULL)
		return uc_mgr_scan_master_configs(list);
	pthread_mutex_lock(&uc_mgr->mutex);
	if (strcmp(identifier, "_verbs") == 0)
		err = get_verb_list(uc_mgr, list);
        else if (strcmp(identifier, "_enadevs") == 0)
        	err = get_enabled_device_list(uc_mgr, list);
        else if (strcmp(identifier, "_enamods") == 0)
                err = get_enabled_modifier_list(uc_mgr, list);
        else {
                str1 = strchr(identifier, '/');
                if (str1) {
                        str = strdup(str1 + 1);
                	if (str == NULL) {
                  		err = -ENOMEM;
                		goto __end;
                        }
                } else {
                        str = NULL;
                }
        	if (check_identifier(identifier, "_devices"))
          		err = get_device_list(uc_mgr, list, str);
                else if (check_identifier(identifier, "_modifiers"))
                        err = get_modifier_list(uc_mgr, list, str);
                else
                        err = get_value_list(uc_mgr, identifier, list, str);
        	if (str)
        		free(str);
        }
      __end:
	pthread_mutex_unlock(&uc_mgr->mutex);
	return err;
}

static int get_value1(const char **value, struct list_head *value_list,
                      const char *identifier)
{
        struct ucm_value *val;
        struct list_head *pos;
        
	if (!value_list)
		return -ENOENT;

        list_for_each(pos, value_list) {
              val = list_entry(pos, struct ucm_value, list);
              if (check_identifier(identifier, val->name)) {
                      *value = strdup(val->data);
                      if (*value == NULL)
                              return -ENOMEM;
                      return 0;
              }
        }
        return -ENOENT;
}

static int get_value3(const char **value,
		      const char *identifier,
		      struct list_head *value_list1,
		      struct list_head *value_list2,
		      struct list_head *value_list3)
{
	int err;

	err = get_value1(value, value_list1, identifier);
	if (err >= 0 || err != -ENOENT)
		return err;
	err = get_value1(value, value_list2, identifier);
	if (err >= 0 || err != -ENOENT)
		return err;
	err = get_value1(value, value_list3, identifier);
	if (err >= 0 || err != -ENOENT)
		return err;
	return -ENOENT;
}

/**
 * \brief Get value
 * \param uc_mgr Use case manager
 * \param identifier Value identifier (string)
 * \param value Returned value string
 * \param item Modifier or Device name (string)
 * \return Zero on success (value is filled), otherwise a negative error code
 */
static int get_value(snd_use_case_mgr_t *uc_mgr,
			const char *identifier,
			const char **value,
			const char *item)
{
	struct use_case_modifier *mod;
	struct use_case_device *dev;
	int err;

	if (item != NULL) {
		mod = find_modifier(uc_mgr, item);
		if (mod != NULL) {
			err = get_value1(value, &mod->value_list, identifier);
			if (err >= 0 || err != -ENOENT)
				return err;
		}
		dev = find_device(uc_mgr->active_verb, item);
		if (dev != NULL) {
			err = get_value1(value, &dev->value_list, identifier);
			if (err >= 0 || err != -ENOENT)
				return err;
		}
	}
	err = get_value1(value, &uc_mgr->active_verb->value_list, identifier);
	if (err >= 0 || err != -ENOENT)
		return err;
	err = get_value1(value, &uc_mgr->value_list, identifier);
	if (err >= 0 || err != -ENOENT)
		return err;
	return -ENOENT;
}

/**
 * \brief Get current - string
 * \param uc_mgr Use case manager
 * \param identifier 
 * \param value Value pointer
 * \return Zero if success, otherwise a negative error code
 *
 * Note: String is dynamically allocated, use free() to
 * deallocate this string.
 */      
int snd_use_case_get(snd_use_case_mgr_t *uc_mgr,
		     const char *identifier,
		     const char **value)
{
        char *str, *str1;
        int err;

	pthread_mutex_lock(&uc_mgr->mutex);
	if (identifier == NULL) {
	        *value = strdup(uc_mgr->card_name);
	        if (*value == NULL) {
	                err = -ENOMEM;
	                goto __end;
                }
                err = 0;
        } else if (strcmp(identifier, "_verb") == 0) {
                if (uc_mgr->active_verb == NULL)
                        return -ENOENT;
                *value = strdup(uc_mgr->active_verb->name);
                if (*value == NULL) {
                        err = -ENOMEM;
                        goto __end;
                }
	        err = 0;
        } else {
                str1 = strchr(identifier, '/');
                if (str1) {
                        str = strdup(str1 + 1);
                	if (str == NULL) {
                  		err = -ENOMEM;
                		goto __end;
                        }
                } else {
                        str = NULL;
                }
                err = get_value(uc_mgr, identifier, value, str);
                if (str)
                        free(str);
        }
      __end:
	pthread_mutex_unlock(&uc_mgr->mutex);
        return err;
}

long device_status(snd_use_case_mgr_t *uc_mgr,
                   const char *device_name)
{
        struct use_case_device *dev;
        struct list_head *pos;
        
        list_for_each(pos, &uc_mgr->active_devices) {
                dev = list_entry(pos, struct use_case_device, active_list);
                if (strcmp(dev->name, device_name) == 0)
                        return 1;
        }
        return 0;
}

long modifier_status(snd_use_case_mgr_t *uc_mgr,
                     const char *modifier_name)
{
        struct use_case_modifier *mod;
        struct list_head *pos;
        
        list_for_each(pos, &uc_mgr->active_modifiers) {
                mod = list_entry(pos, struct use_case_modifier, active_list);
                if (strcmp(mod->name, modifier_name) == 0)
                        return 1;
        }
        return 0;
}


/**
 * \brief Get current - integer
 * \param uc_mgr Use case manager
 * \param identifier 
 * \return Value if success, otherwise a negative error code 
 */
int snd_use_case_geti(snd_use_case_mgr_t *uc_mgr,
		      const char *identifier,
		      long *value)
{
        char *str, *str1;
        long err;

	pthread_mutex_lock(&uc_mgr->mutex);
        if (0) {
                /* nothing here - prepared for fixed identifiers */
        } else {
                str1 = strchr(identifier, '/');
                if (str1) {
                        str = strdup(str1 + 1);
                	if (str == NULL) {
                  		err = -ENOMEM;
                		goto __end;
                        }
                } else {
                        str = NULL;
                }
                if (check_identifier(identifier, "_devstatus")) {
                        err = device_status(uc_mgr, str);
			if (err >= 0) {
				*value = err;
				err = 0;
			}
		} else if (check_identifier(identifier, "_modstatus")) {
                        err = modifier_status(uc_mgr, str);
			if (err >= 0) {
				*value = err;
				err = 0;
			}
		} else
                        err = -EINVAL;
                if (str)
                        free(str);
        }
      __end:
	pthread_mutex_unlock(&uc_mgr->mutex);
        return err;
}

static int handle_transition_verb(snd_use_case_mgr_t *uc_mgr,
                                  struct use_case_verb *new_verb)
{
        struct list_head *pos;
        struct transition_sequence *trans;
        int err;

        list_for_each(pos, &uc_mgr->active_verb->transition_list) {
                trans = list_entry(pos, struct transition_sequence, list);
                if (strcmp(trans->name, new_verb->name) == 0) {
                        err = execute_sequence(uc_mgr, &trans->transition_list,
					       &uc_mgr->active_verb->value_list,
					       &uc_mgr->value_list,
					       NULL);
                        if (err >= 0)
                                return 1;
                        return err;
                }
        }
        return 0;
}

static int set_verb_user(snd_use_case_mgr_t *uc_mgr,
                         const char *verb_name)
{
        struct use_case_verb *verb;
        int err;

        if (uc_mgr->active_verb &&
            strcmp(uc_mgr->active_verb->name, verb_name) == 0)
                return 0;
        if (strcmp(verb_name, SND_USE_CASE_VERB_INACTIVE) != 0) {
                verb = find_verb(uc_mgr, verb_name);
                if (verb == NULL)
                        return -ENOENT;
        } else {
                verb = NULL;
        }
        if (uc_mgr->active_verb) {
                err = handle_transition_verb(uc_mgr, verb);
                if (err == 0) {
                        err = dismantle_use_case(uc_mgr);
                        if (err < 0)
                                return err;
                } else if (err == 1) {
                        uc_mgr->active_verb = verb;
                        verb = NULL;
                } else {
                        verb = NULL; /* show error */
                }
        }
        if (verb) {
                err = set_verb(uc_mgr, verb, 1);
                if (err < 0)
                        uc_error("error: failed to initialize new use case: %s",
                                 verb_name);
        }
        return err;
}


static int set_device_user(snd_use_case_mgr_t *uc_mgr,
                           const char *device_name,
                           int enable)
{
        struct use_case_device *device;

        if (uc_mgr->active_verb == NULL)
                return -ENOENT;
        device = find_device(uc_mgr->active_verb, device_name);
        if (device == NULL)
                return -ENOENT;
        return set_device(uc_mgr, device, enable);
}

static int set_modifier_user(snd_use_case_mgr_t *uc_mgr,
                             const char *modifier_name,
                             int enable)
{
        struct use_case_modifier *modifier;

        if (uc_mgr->active_verb == NULL)
                return -ENOENT;

        modifier = find_modifier(uc_mgr, modifier_name);
        if (modifier == NULL)
                return -ENOENT;
        return set_modifier(uc_mgr, modifier, enable);
}

static int switch_device(snd_use_case_mgr_t *uc_mgr,
                         const char *old_device,
                         const char *new_device)
{
        struct use_case_device *xold, *xnew;
        struct transition_sequence *trans;
        struct list_head *pos;
        int err, seq_found = 0;
        
        if (uc_mgr->active_verb == NULL)
                return -ENOENT;
        if (device_status(uc_mgr, old_device) == 0) {
                uc_error("error: device %s not enabled", old_device);
                return -EINVAL;
        }
        if (device_status(uc_mgr, new_device) != 0) {
                uc_error("error: device %s already enabled", new_device);
                return -EINVAL;
        }
        xold = find_device(uc_mgr->active_verb, old_device);
        if (xold == NULL)
                return -ENOENT;
        xnew = find_device(uc_mgr->active_verb, new_device);
        if (xnew == NULL)
                return -ENOENT;
        err = 0;
        list_for_each(pos, &xold->transition_list) {
                trans = list_entry(pos, struct transition_sequence, list);
                if (strcmp(trans->name, new_device) == 0) {
                        err = execute_sequence(uc_mgr, &trans->transition_list,
					       &xold->value_list,
					       &uc_mgr->active_verb->value_list,
					       &uc_mgr->value_list);
                        if (err >= 0) {
                                list_del(&xold->active_list);
                                list_add_tail(&xnew->active_list, &uc_mgr->active_devices);
                        }
                        seq_found = 1;
                        break;
                }
        }
        if (!seq_found) {
                err = set_device(uc_mgr, xold, 0);
                if (err < 0)
                        return err;
                err = set_device(uc_mgr, xnew, 1);
                if (err < 0)
                        return err;
        }
        return err;
}

static int switch_modifier(snd_use_case_mgr_t *uc_mgr,
                           const char *old_modifier,
                           const char *new_modifier)
{
        struct use_case_modifier *xold, *xnew;
        struct transition_sequence *trans;
        struct list_head *pos;
        int err, seq_found = 0;
        
        if (uc_mgr->active_verb == NULL)
                return -ENOENT;
        if (modifier_status(uc_mgr, old_modifier) == 0) {
                uc_error("error: modifier %s not enabled", old_modifier);
                return -EINVAL;
        }
        if (modifier_status(uc_mgr, new_modifier) != 0) {
                uc_error("error: modifier %s already enabled", new_modifier);
                return -EINVAL;
        }
        xold = find_modifier(uc_mgr, old_modifier);
        if (xold == NULL)
                return -ENOENT;
        xnew = find_modifier(uc_mgr, new_modifier);
        if (xnew == NULL)
                return -ENOENT;
        err = 0;
        list_for_each(pos, &xold->transition_list) {
                trans = list_entry(pos, struct transition_sequence, list);
                if (strcmp(trans->name, new_modifier) == 0) {
                        err = execute_sequence(uc_mgr, &trans->transition_list,
					       &xold->value_list,
					       &uc_mgr->active_verb->value_list,
					       &uc_mgr->value_list);
                        if (err >= 0) {
                                list_del(&xold->active_list);
                                list_add_tail(&xnew->active_list, &uc_mgr->active_modifiers);
                        }
                        seq_found = 1;
                        break;
                }
        }
        if (!seq_found) {
                err = set_modifier(uc_mgr, xold, 0);
                if (err < 0)
                        return err;
                err = set_modifier(uc_mgr, xnew, 1);
                if (err < 0)
                        return err;
        }
        return err;        
}

/**
 * \brief Set new
 * \param uc_mgr Use case manager
 * \param identifier
 * \param value Value
 * \return Zero if success, otherwise a negative error code
 */
int snd_use_case_set(snd_use_case_mgr_t *uc_mgr,
                     const char *identifier,
                     const char *value)
{
	char *str, *str1;
	int err;

	pthread_mutex_lock(&uc_mgr->mutex);
	if (strcmp(identifier, "_verb") == 0)
	        err = set_verb_user(uc_mgr, value);
        else if (strcmp(identifier, "_enadev") == 0)
                err = set_device_user(uc_mgr, value, 1);
        else if (strcmp(identifier, "_disdev") == 0)
                err = set_device_user(uc_mgr, value, 0);
        else if (strcmp(identifier, "_enamod") == 0)
                err = set_modifier_user(uc_mgr, value, 1);
        else if (strcmp(identifier, "_dismod") == 0)
                err = set_modifier_user(uc_mgr, value, 0);
        else {
                str1 = strchr(identifier, '/');
                if (str1) {
                        str = strdup(str1 + 1);
                	if (str == NULL) {
                  		err = -ENOMEM;
                		goto __end;
                        }
                } else {
                        str = NULL;
                }
                if (check_identifier(identifier, "_swdev"))
                        err = switch_device(uc_mgr, str, value);
                else if (check_identifier(identifier, "_swmod"))
                        err = switch_modifier(uc_mgr, str, value);
                else
                        err = -EINVAL;
                if (str)
                        free(str);
        }
      __end:
	pthread_mutex_unlock(&uc_mgr->mutex);
        return err;
}
