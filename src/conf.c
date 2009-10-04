/* -*- mode: c; coding: utf-8; c-file-style: "K&R"; tab-width: 8; indent-tabs-mode: t; -*- */

/**
 * raidxor_try_configure_raid() - configures the raid
 *
 * Checks, if enough information was supplied through sysfs and if so,
 * completes the configuration with the data.
 */
static void raidxor_try_configure_raid(raidxor_conf_t *conf) {
	resource_t **resources;
	disk_info_t *unit;
	unsigned int i, j;
	char buffer[32];
	unsigned long flags;
	mddev_t *mddev = conf->mddev;

	if (!conf || !mddev) {
		printk(KERN_INFO "raidxor: NULL pointer in "
		       "raidxor_free_conf\n");
		return;
	}

	if (conf->units_per_resource <= 0) {
		printk(KERN_INFO "raidxor: need units per resource: %u\n",
		       conf->units_per_resource);
		goto out;
	}

	if (conf->n_units % conf->units_per_resource != 0) {
		printk(KERN_INFO
		       "raidxor: parameters don't match %u %% %u != 0\n",
		       conf->n_units,
		       conf->units_per_resource);
		goto out;
	}

	conf->n_data_units = 0;
	for (i = 0; i < conf->n_units; ++i) {
		if (conf->units[i].redundant == -1) {
			printk(KERN_INFO
			       "raidxor: unit %u, %s is not initialized\n",
			       i, bdevname(conf->units[i].rdev->bdev, buffer));
			goto out;
		}

		if (conf->units[i].redundant == 0)
			++conf->n_data_units;
	}

	printk(KERN_INFO "raidxor: got enough information, building raid\n");

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

	for (i = 0; i < conf->n_resources; ++i) {
		resources[i]->n_units = conf->units_per_resource;
		for (j = 0; j < conf->units_per_resource; ++j) {
			unit = &conf->units[i + j * conf->n_resources];

			unit->resource = resources[i];
			resources[i]->units[j] = unit;
		}
	}


	/* allocate the cache with a default of 10 lines;
	   TODO: could be a driver option, or allow for shrinking/growing ... */
	/* one chunk is CHUNK_SIZE / PAGE_SIZE pages long, eqv. >> PAGE_SHIFT */
	conf->cache = raidxor_alloc_cache(number_of_cache_lines,
					  conf->n_data_units,
					  conf->n_units - conf->n_data_units,
					  conf->chunk_size >> PAGE_SHIFT);
	if (!conf->cache)
		goto out_free_resources;
	conf->cache->conf = conf;
	conf->trap1 = conf->trap2 = conf->trap3 = 42;

	/* now a request is between 4096 and N_DATA_UNITS * CHUNK_SIZE bytes long */
	printk(KERN_INFO "and max sectors to %lu\n",
	       (conf->chunk_size >> 9) * conf->n_data_units);
	blk_queue_max_sectors(mddev->queue,
			      (conf->chunk_size >> 9) * conf->n_data_units);
	blk_queue_segment_boundary(mddev->queue,
				   (conf->chunk_size >> 1) *
				   conf->n_data_units - 1);

	printk(KERN_INFO "setting device size\n");

	/* since all stripes are equally long */
	mddev->array_sectors = conf->n_data_units * mddev->size * 2;
	set_capacity(mddev->gendisk, mddev->array_sectors);

	printk (KERN_INFO "raidxor: array_sectors is %u * %llu= "
		"%llu blocks, %llu sectors\n",
		(unsigned int) conf->n_data_units,
		(unsigned long long) mddev->size * 2,
		(unsigned long long) mddev->array_sectors,
		(unsigned long long) mddev->array_sectors / 2);

	conf->resources = resources;

	WITHLOCKCONF(conf, flags, {
	clear_bit(CONF_INCOMPLETE, &conf->flags);
	});

	return;
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
raidxor_show_decoding(mddev_t *mddev, char *page)
{
	return -EIO;
}

static ssize_t
raidxor_store_decoding(mddev_t *mddev, const char *page, size_t len)
{
	unsigned char length, i, red, ntemps, temp;
	decoding_t *decoding;
	raidxor_conf_t *conf = mddev_to_conf(mddev);
	unsigned long flags = 0;
	unsigned char index = 0, temporary = 0;
	size_t oldlen = len;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (!conf)
		return -ENODEV;

	ntemps = *page++;
	--len;

	if (raidxor_cache_ensure_temps(conf, conf->n_enc_temps, ntemps))
		goto out;

	for (; len >= 1;) {
		index = *page++;
		--len;

		temporary = *page++;
		--len;

		if (temporary > 1) {
			printk(KERN_INFO "temporary != (0 | 1) == %u\n", temporary);
			return -EINVAL;
		}

		if (index >= conf->n_units)
			goto out;

		if (len < 1)
			goto out;

		length = *page++;
		--len;

		if (length > len)
			goto out;

		decoding = kzalloc(sizeof(decoding_t) +
				   sizeof(coding_t) * length, GFP_NOIO);
		if (!decoding)
			goto out;
		decoding->n_units = length;

		for (i = 0; i < length; ++i) {
			temp = *page++;
			--len;

			red = *page++;
			--len;

			if (temp) {
				if (red >= conf->n_dec_temps)
					goto out_free_decoding;

				decoding->units[i].decoding = conf->dec_temps[red];
			}
			else {
				if (red >= conf->n_units)
					goto out_free_decoding;

				decoding->units[i].disk = &conf->units[red];
			}
			decoding->units[i].temporary = temp;
		}

		WITHLOCKCONF(conf, flags, {
		if (temporary) {
			if (conf->dec_temps[index])
				kfree(conf->dec_temps[index]);
			conf->dec_temps[index] = decoding;
		}
		else {
			raidxor_safe_free_decoding(&conf->units[index]);
			conf->units[index].decoding = decoding;
		}
		});

		printk(KERN_INFO "read decoding info for index %d%s\n", index, temporary ? ", temporary" : "");
	}

	return oldlen;
out_free_decoding:
	kfree(decoding);
out:
	printk(KERN_INFO "aborting from index %d%s\n", index, temporary ? ", temporary" : "");
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
	unsigned char index, redundant, length, i, red, ntemps, temp;
	encoding_t *encoding;
	size_t oldlen = len;
	unsigned long flags = 0;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (!conf)
		return -ENODEV;

	ntemps = *page++;
	--len;

	if (raidxor_cache_ensure_temps(conf, ntemps, conf->n_dec_temps))
		goto out;

	for (; len >= 2;) {
		index = *page++;
		--len;

		redundant = *page++;
		--len;

		if (redundant > 2) {
			printk(KERN_INFO "redundant != (0 | 1 | 2) == %u\n", redundant);
			return -EINVAL;
		}

		if (redundant != 2 && index >= conf->n_units) {
			printk(KERN_INFO "index out of bounds %u >= %u\n", index, conf->n_units);
			goto out;
		}
		else if (redundant == 2 && index >= conf->n_enc_temps) {
			printk(KERN_INFO "index out of bounds %u >= %u\n", index, conf->n_enc_temps);
			goto out;
		}

		if (redundant != 2)
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
				   sizeof(coding_t) * length, GFP_NOIO);
		if (!encoding)
			goto out_reset;
		encoding->n_units = length;

		for (i = 0; i < length; ++i) {
			temp = *page++;
			--len;

			red = *page++;
			--len;

			if (temp) {
				if (red >= conf->n_enc_temps)
					goto out_free_encoding;

				encoding->units[i].encoding = conf->enc_temps[red];
			}
			else {
				if (red >= conf->n_units)
					goto out_free_encoding;

				encoding->units[i].disk = &conf->units[red];
			}
			encoding->units[i].temporary = temp;
		}

		WITHLOCKCONF(conf, flags, {
		if (redundant == 2) {
			if (conf->enc_temps[index])
				kfree(conf->enc_temps[index]);
			conf->enc_temps[index] = encoding;
		}
		else {
			raidxor_safe_free_encoding(&conf->units[index]);
			conf->units[index].encoding = encoding;
		}
		});

		printk(KERN_INFO "raidxor: read redundant unit encoding info for unit %u\n", index);
	}

	return oldlen;
out_free_encoding:
	kfree(encoding);
out_reset:
	if (redundant != 2)
		conf->units[index].redundant = -1;
out:
	return -EINVAL;
}

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

#if 0
	for (i = 0; i < conf->n_units; ++i) {
		seq_printf(seq, "unit %u: encoding = %p, decoding %p\n", i,
			   conf->units[i].encoding, conf->units[i].decoding);
		if (conf->units[i].encoding && conf->units[i].encoding->units) {
			seq_printf(seq, "  encoding %d units\n", conf->units[i].encoding->n_units);
			for (j = 0; j <conf->units[i].encoding->n_units; ++j) {
				seq_printf(seq, "    entry %d: %d, disk %p, encoding %p\n", j,
					   conf->units[i].encoding->units[j].temporary,
					   conf->units[i].encoding->units[j].disk,
					   conf->units[i].encoding->units[j].encoding);
			}
		}
		if (conf->units[i].decoding && conf->units[i].decoding->units) {
			seq_printf(seq, "  decoding %d units\n", conf->units[i].decoding->n_units);
			for (j = 0; j < conf->units[i].decoding->n_units; ++j) {
				seq_printf(seq, "    entry %d: %d, disk %p, decoding %p\n", j,
					   conf->units[i].decoding->units[j].temporary,
					   conf->units[i].decoding->units[j].disk,
					   conf->units[i].decoding->units[j].decoding);
			}
		}
	}

	for (i = 0; i < conf->n_enc_temps; ++i) {
		seq_printf(seq, "enc_temp %u: encoding = %p\n", i,
			   conf->enc_temps[i]);
	}

	for (i = 0; i < conf->n_dec_temps; ++i) {
		seq_printf(seq, "dec_temp %u: decoding = %p\n", i,
			   conf->dec_temps[i]);
	}
#endif

	for (i = 0; i < conf->cache->n_lines; ++i) {
		seq_printf(seq, "line %u: %s at sector %llu\n", i,
			   raidxor_cache_line_status(conf->cache->lines[i]),
			   (unsigned long long) conf->cache->lines[i]->sector);
	}
}

#if 0
Local variables:
c-basic-offset: 8
End:
#endif
