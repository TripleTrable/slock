/* user and group to drop privileges to */
static const char *user  = "lars";
static const char *group = "users";

static const char *colorname[NUMCOLS] = {
	[INIT] =   "black",     /* after initialization */
	[INPUT] =  "#005577",   /* during input */
	[FAILED] = "#CC3333",   /* wrong password */
};

/* treat a cleared input like a wrong password (color) */
static const int failonclear = 1;

/*Set Blur radius*/
static const int blurRadius=0;
static const int pixelSize=5;
