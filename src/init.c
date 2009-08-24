#ifdef RAIDXOR_RUN_TESTCASES
#include "tests.c"
#endif

static struct mdk_personality raidxor_personality =
{
	.name         = "raidxor",
	.level        = LEVEL_XOR,
	.owner        = THIS_MODULE,

	.make_request = raidxor_make_request,
	.run          = raidxor_run,
	.stop         = raidxor_stop,
	.status       = raidxor_status,

	/* handles faulty disks, so we have to implement this one */
	.error_handler = raidxor_error,

	/* .sync_request = raidxor_sync_request, */
	/* .quiesce = raidxor_quiesce, */
};

static int __init raidxor_init(void)
{
	#ifdef RAIDXOR_RUN_TESTCASES
	if (raidxor_run_test_cases())
		return -1;
	#endif

	return register_md_personality(&raidxor_personality);
}

static void __exit raidxor_exit(void)
{
	unregister_md_personality(&raidxor_personality);
}

MODULE_AUTHOR("Olof-Joachim Frahm");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("md");
MODULE_DESCRIPTION("Raid module with parameterisation support for en- and decoding.");

module_init( raidxor_init );
module_exit( raidxor_exit );
