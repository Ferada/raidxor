/* -*- mode: c; coding: utf-8; c-file-style: "K&R"; tab-width: 8; indent-tabs-mode: t; -*- */

/**
 * raidxor_try_configure_raid() - configures the raid
 *
 * Checks, if enough information was supplied through sysfs and if so,
 * completes the configuration with the data.
 */
static void raidxor_try_configure_raid(raidxor_conf_t *conf) {
	resource_t **resources;
	stripe_t **stripes;
	disk_info_t *unit;
	unsigned int i, j, old_data_units = 0;
	char buffer[32];
	mddev_t *mddev = conf->mddev;

	if (!conf || !mddev) {
		printk(KERN_EMERG "raidxor: NULL pointer in "
		       "raidxor_free_conf\n");
		return;
	}

	if (conf->resources_per_stripe <= 0 || conf->units_per_resource <= 0) {
		printk(KERN_EMERG "raidxor: need resources per stripe or "
		       "units per resource: %u or %u\n",
		       conf->resources_per_stripe, conf->units_per_resource);
		goto out;
	}

	if (conf->n_units % (conf->resources_per_stripe *
			     conf->units_per_resource) != 0) {
		printk(KERN_EMERG
		       "raidxor: parameters don't match %u %% (%u * %u) != 0\n",
		       conf->n_units, conf->resources_per_stripe,
		       conf->units_per_resource);
		goto out;
	}

	for (i = 0; i < conf->n_units; ++i) {
		if (conf->units[i].redundant == -1) {
			printk(KERN_EMERG
			       "raidxor: unit %u, %s is not initialized\n",
			       i, bdevname(conf->units[i].rdev->bdev, buffer));
			goto out;
		}
	}

	printk(KERN_EMERG "raidxor: got enough information, building raid\n");

	conf->n_resources = conf->n_units / conf->units_per_resource;

	resources = kzalloc(sizeof(resource_t *) * conf->n_resources,
			    GFP_KERNEL);
	if (!resources)
		goto out;

	for (i = 0; i < conf->n_resources; ++i) {
		resources[i] = kzalloc(sizeof(resource_t) +
				       (sizeof(disk_info_t *) *
					conf->units_per_resource), GFP_KERNEL);
		if (!resources[i])
			goto out_free_resources;
	}

	conf->n_stripes = conf->n_units /
		(conf->resources_per_stripe * conf->units_per_resource);

	stripes = kzalloc(sizeof(stripe_t *) * conf->n_stripes, GFP_KERNEL);
	if (!stripes)
		goto out_free_resources;

	for (i = 0; i < conf->n_stripes; ++i) {
		stripes[i] = kzalloc(sizeof(stripe_t) +
				     (sizeof(disk_info_t *) *
				      conf->resources_per_stripe *
				      conf->units_per_resource), GFP_KERNEL);
		if (!stripes[i])
			goto out_free_stripes;
	}

	for (i = 0; i < conf->n_resources; ++i) {
		resources[i]->n_units = conf->units_per_resource;
		for (j = 0; j < conf->units_per_resource; ++j) {
			unit = &conf->units[i + j * conf->n_resources];

			unit->resource = resources[i];
			resources[i]->units[j] = unit;
		}
	}

	printk(KERN_EMERG "now calculating stripes and sizes\n");

	for (i = 0; i < conf->n_stripes; ++i) {
		printk(KERN_EMERG "direct: stripes[%u] %p\n", i, stripes[i]);
		stripes[i]->n_units = conf->resources_per_stripe *
			conf->units_per_resource;
		printk(KERN_EMERG "using %d units per stripe\n", stripes[i]->n_units);

		for (j = 0; j < stripes[i]->n_units; ++j) {
			printk(KERN_EMERG "using unit %u for stripe %u, index %u\n",
			       i * conf->units_per_resource * conf->resources_per_stripe + j, i, j);
			unit = &conf->units[i * conf->units_per_resource * conf->resources_per_stripe + j];

			unit->stripe = stripes[i];

			if (unit->redundant == 0)
				++stripes[i]->n_data_units;
			stripes[i]->units[j] = unit;
		}

		if (old_data_units == 0) {
			old_data_units = stripes[i]->n_data_units;
		}
		else if (old_data_units != stripes[i]->n_data_units) {
			printk(KERN_EMERG "number of data units on two stripes"
			       " are different: %u on stripe %d where we"
			       " assumed %u\n",
			       i, stripes[i]->n_data_units, old_data_units);
			goto out_free_stripes;
		}

		stripes[i]->size = stripes[i]->n_data_units * mddev->size * 2;
	}

	/* allocate the cache with a default of 10 lines;
	   TODO: could be a driver option, or allow for shrinking/growing ... */
	/* one chunk is CHUNK_SIZE / PAGE_SIZE pages long, eqv. >> PAGE_SHIFT */
	conf->cache = raidxor_alloc_cache(number_of_cache_lines,
					  stripes[0]->n_data_units,
					  stripes[0]->n_units -
					  stripes[0]->n_data_units,
					  conf->chunk_size >> PAGE_SHIFT);
	if (!conf->cache)
		goto out_free_stripes;
	conf->cache->conf = conf;

	/* now a request is between 4096 and N_DATA_UNITS * CHUNK_SIZE bytes long */
	printk(KERN_EMERG "and max sectors to %lu\n",
	       (conf->chunk_size >> 9) * stripes[0]->n_data_units);
	blk_queue_max_sectors(mddev->queue,
			      (conf->chunk_size >> 9) * stripes[0]->n_data_units);
	blk_queue_segment_boundary(mddev->queue,
				   (conf->chunk_size >> 1) *
				   stripes[0]->n_data_units - 1);

	printk(KERN_EMERG "setting device size\n");

	/* since all stripes are equally long */
	mddev->array_sectors = stripes[0]->size * conf->n_stripes;
	set_capacity(mddev->gendisk, mddev->array_sectors);

	printk (KERN_EMERG "raidxor: array_sectors is %llu * %u = "
		"%llu blocks, %llu sectors\n",
		(unsigned long long) stripes[0]->size,
		(unsigned int) conf->n_stripes,
		(unsigned long long) mddev->array_sectors,
		(unsigned long long) mddev->array_sectors / 2);

	conf->stripe_size = stripes[0]->size;
	conf->resources = resources;
	conf->stripes = stripes;
	conf->configured = 1;

	return;
out_free_stripes:
	for (i = 0; i < conf->n_stripes; ++i)
		kfree(stripes[i]);
	kfree(stripes);
out_free_resources:
	for (i = 0; i < conf->n_resources; ++i)
		kfree(resources[i]);
	kfree(resources);
out:
	return;
}

static ssize_t
raidxor_show_units_per_resource(mddev_t *mddev, char *page)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);

	if (conf)
		return sprintf(page, "%u\n", conf->units_per_resource);
	else
		return -ENODEV;
}

static ssize_t
raidxor_store_units_per_resource(mddev_t *mddev, const char *page, size_t len)
{
	unsigned long new, flags = 0;
	raidxor_conf_t *conf = mddev_to_conf(mddev);

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (!conf)
		return -ENODEV;

	strict_strtoul(page, 10, &new);

	if (new == 0)
		return -EINVAL;

	WITHLOCKCONF(conf, flags, {
	raidxor_safe_free_conf(conf);
	conf->units_per_resource = new;
	});

	raidxor_try_configure_raid(conf);

	return len;
}

static ssize_t
raidxor_show_resources_per_stripe(mddev_t *mddev, char *page)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);

	if (conf)
		return sprintf(page, "%u\n", conf->resources_per_stripe);
	else
		return -ENODEV;
}

static ssize_t
raidxor_store_resources_per_stripe(mddev_t *mddev, const char *page, size_t len)
{
	unsigned long new, flags = 0;
	raidxor_conf_t *conf = mddev_to_conf(mddev);

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (!conf)
		return -ENODEV;

	strict_strtoul(page, 10, &new);

	if (new == 0)
		return -EINVAL;

	WITHLOCKCONF(conf, flags, {
	raidxor_safe_free_conf(conf);
	conf->resources_per_stripe = new;
	});

	raidxor_try_configure_raid(conf);

	return len;
}

static ssize_t
raidxor_show_decoding(mddev_t *mddev, char *page)
{
	return -EIO;
}

static ssize_t
raidxor_store_decoding(mddev_t *mddev, const char *page, size_t len)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	unsigned char index, length, i, red;
	decoding_t *decoding;
	unsigned long flags = 0;
	size_t oldlen = len;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (!conf)
		return -ENODEV;

	for (; len >= 1;) {
		index = *page++;
		--len;

		if (index >= conf->n_units)
			goto out;

		if (len < 1)
			goto out;

		length = *page++;
		--len;

		if (length > len)
			goto out;

		decoding = kzalloc(sizeof(decoding_t) +
				   sizeof(disk_info_t *) * length, GFP_NOIO);
		if (!decoding)
			goto out;
		decoding->n_units = length;

		for (i = 0; i < length; ++i) {
			red = *page++;
			--len;

			if (red >= conf->n_units)
				goto out_free_decoding;

			decoding->units[i] = &conf->units[red];
		}

		WITHLOCKCONF(conf, flags, {
		raidxor_safe_free_decoding(&conf->units[index]);
		conf->units[index].decoding = decoding;
		});

		printk(KERN_INFO "read decoding info\n");
	}

	return oldlen;
out_free_decoding:
	kfree(decoding);
out:
	return -EINVAL;
}

static ssize_t
raidxor_show_encoding(mddev_t *mddev, char *page)
{
	return -EIO;
}

static ssize_t
raidxor_store_encoding(mddev_t *mddev, const char *page, size_t len)
{
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	unsigned char index, redundant, length, i, red;
	encoding_t *encoding;
	size_t oldlen = len;
	unsigned long flags = 0;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (!conf)
		return -ENODEV;

	for (; len >= 2;) {
		index = *page++;
		--len;

		if (index >= conf->n_units) {
			printk(KERN_INFO "index out of bounds %u >= %u\n", index, conf->n_units);
			goto out;
		}

		redundant = *page++;
		--len;

		if (redundant != 0 && redundant != 1) {
			printk(KERN_INFO "redundant != (0 | 1) == %u\n", redundant);
			return -EINVAL;
		}

		conf->units[index].redundant = redundant;

		if (redundant == 0) {
			printk(KERN_INFO "read non-redundant unit info\n");
			continue;
		}

		if (len < 1)
			goto out_reset;

		length = *page++;
		--len;

		if (length > len)
			goto out_reset;

		encoding = kzalloc(sizeof(encoding_t) +
				   sizeof(disk_info_t *) * length, GFP_NOIO);
		if (!encoding)
			goto out_reset;
		encoding->n_units = length;

		for (i = 0; i < length; ++i) {
			red = *page++;
			--len;

			if (red >= conf->n_units)
				goto out_free_encoding;

			encoding->units[i] = &conf->units[red];
		}

		WITHLOCKCONF(conf, flags, {
		raidxor_safe_free_encoding(&conf->units[index]);
		conf->units[index].encoding = encoding;
		});

		printk(KERN_INFO "raidxor: read redundant unit encoding info for unit %u\n", index);
	}

	return oldlen;
out_free_encoding:
	kfree(encoding);
out_reset:
	conf->units[index].redundant = -1;
out:
	return -EINVAL;
}

static struct md_sysfs_entry
raidxor_resources_per_stripe = __ATTR(resources_per_stripe, S_IRUGO | S_IWUSR,
				     raidxor_show_resources_per_stripe,
				     raidxor_store_resources_per_stripe);

static struct md_sysfs_entry
raidxor_units_per_resource = __ATTR(units_per_resource, S_IRUGO | S_IWUSR,
				    raidxor_show_units_per_resource,
				    raidxor_store_units_per_resource);

static struct md_sysfs_entry
raidxor_encoding = __ATTR(encoding, S_IRUGO | S_IWUSR,
			  raidxor_show_encoding,
			  raidxor_store_encoding);

static struct md_sysfs_entry
raidxor_decoding = __ATTR(decoding, S_IRUGO | S_IWUSR,
			  raidxor_show_decoding,
			  raidxor_store_decoding);

static struct attribute * raidxor_attrs[] = {
	(struct attribute *) &raidxor_resources_per_stripe,
	(struct attribute *) &raidxor_units_per_resource,
	(struct attribute *) &raidxor_encoding,
	(struct attribute *) &raidxor_decoding,
	NULL
};

static struct attribute_group raidxor_attrs_group = {
	.name = NULL,
	.attrs = raidxor_attrs,
};

static void raidxor_status(struct seq_file *seq, mddev_t *mddev)
{
	unsigned int i;
	raidxor_conf_t *conf = mddev_to_conf(mddev);

	seq_printf(seq, "\n");

	for (i = 0; i < conf->n_stripes; ++i) {
		seq_printf(seq, "stripe %u with size %llu\n", i,
			   (unsigned long long) conf->stripes[i]->size);
	}

	for (i = 0; i < conf->cache->n_lines; ++i) {
		seq_printf(seq, "line %u: %s at sector %llu\n", i,
			   raidxor_cache_line_status(conf->cache->lines[i]),
			   (unsigned long long) conf->cache->lines[i]->sector);
	}

	/* seq_printf(seq, " I'm feeling fine"); */
	return;
}

#if 0
Local variables:
c-basic-offset: 8
End:
#endif
