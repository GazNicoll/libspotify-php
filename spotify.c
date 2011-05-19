#include "php_spotify.h"

zend_class_entry *spotify_ce;
zend_object_handlers spotify_object_handlers;
static int is_logged_in, is_logged_out;

static void playlistcontainer_complete(sp_playlistcontainer *pc, void *userdata);
static void logged_in(sp_session *session, sp_error error);
static void logged_out(sp_session *session);
static void log_message(sp_session *session, const char *data);

void (*metadata_updated_fn)(void);

static void metadata_updated(sp_session *sess)
{
	if (metadata_updated_fn) {
		metadata_updated_fn();
	}
}

static sp_session_callbacks callbacks = {
	&logged_in,
	&logged_out,
	&metadata_updated,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	&log_message
};

PHP_METHOD(Spotify, __construct)
{
	zval *object = getThis();
	zval *z_key, *z_user, *z_pass;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zzz", &z_key, &z_user, &z_pass) == FAILURE) {
		return;
	}

	sp_session_config config;
	sp_error error;
	sp_session *session;
	FILE *fp;
	long key_size;
	char *key_filename;

	spotify_object *obj = (spotify_object*)zend_object_store_get_object(object TSRMLS_CC);

	memset(&config, 0, sizeof(config));
	config.api_version = SPOTIFY_API_VERSION;
	config.cache_location = "tmp";
	config.settings_location = "tmp";
	config.user_agent = "libspotify-php";

	key_filename = Z_STRVAL_P(z_key);

	fp = fopen(key_filename, "rb");
	if (!fp) {
		zend_throw_exception((zend_class_entry*)zend_exception_get_default(), "unable to open spotify key file", 0 TSRMLS_CC);
		return;
	}

	fseek(fp, 0L, SEEK_END);
	key_size = ftell(fp);
	fseek(fp, 0L, SEEK_SET);

	if (key_size > 4096) {
		fclose(fp);
		zend_throw_exception((zend_class_entry*)zend_exception_get_default(), "key file is way too large to be a key file", 0 TSRMLS_CC);
		return;
	}

	obj->key_data = (char*)emalloc(sizeof(char) * key_size);
	if (fread(obj->key_data, 1, key_size, fp) != key_size) {
		fclose(fp);
		zend_throw_exception((zend_class_entry*)zend_exception_get_default(), "failed reading key file", 0 TSRMLS_CC);
		return;
	}
	fclose(fp);

	config.application_key = obj->key_data;
	config.application_key_size = key_size;

	config.callbacks = &callbacks;

	error = sp_session_create(&config, &session);
	if (SP_ERROR_OK != error) {
		zend_throw_exception((zend_class_entry*)zend_exception_get_default(), "unable to create session", 0 TSRMLS_CC);
		return;
	}

	sp_session_login(session, Z_STRVAL_P(z_user), Z_STRVAL_P(z_pass));

	obj->session = session;
	obj->timeout = 0;
	obj->playlistcontainer = NULL;

	do {
		sp_session_process_events(obj->session, &obj->timeout);
	} while (obj->timeout == 0 || !is_logged_in);
}

PHP_METHOD(Spotify, __destruct)
{
	spotify_object *obj = (spotify_object*)zend_object_store_get_object(getThis() TSRMLS_CC);
	int timeout = 0;

	if (obj->playlistcontainer != NULL) {
		//sp_playlistcontainer_release(obj->playlistcontainer);
	}

    do {
        sp_session_process_events(obj->session, &timeout);
    } while (timeout == 0);

	sp_session_logout(obj->session);

	timeout = 0;
	do {
		sp_session_process_events(obj->session, &timeout);
	} while (!is_logged_out || timeout == 0);

	sp_session_release(obj->session);

	efree(obj->key_data);
}

PHP_METHOD(Spotify, getPlaylists)
{
	zval *thisptr = getThis(), tempretval;
	int i, timeout = 0, num_playlists;

	spotify_object *p = (spotify_object*)zend_object_store_get_object(thisptr TSRMLS_CC);

	SPOTIFY_METHOD(Spotify, initPlaylistContainer, &tempretval, thisptr);

	array_init(return_value);

	num_playlists = sp_playlistcontainer_num_playlists(p->playlistcontainer);
	for (i=0; i<num_playlists; i++) {
		sp_playlist *playlist = sp_playlistcontainer_playlist(p->playlistcontainer, i);

		while (!sp_playlist_is_loaded(playlist)) {
			sp_session_process_events(p->session, &timeout);
		}

		zval *z_playlist;
		ALLOC_INIT_ZVAL(z_playlist);
		object_init_ex(z_playlist, spotifyplaylist_ce);
		SPOTIFY_METHOD2(SpotifyPlaylist, __construct, &tempretval, z_playlist, thisptr, playlist);
		add_next_index_zval(return_value, z_playlist);
	}
}

PHP_METHOD(Spotify, getStarredPlaylist)
{
	zval *object = getThis();
	zval temp;

	spotify_object *obj = (spotify_object*)zend_object_store_get_object(object TSRMLS_CC);

	do {
		sp_session_process_events(obj->session, &obj->timeout);
	} while (obj->timeout == 0);

	sp_playlist *playlist = sp_session_starred_create(obj->session);

	object_init_ex(return_value, spotifyplaylist_ce);
	SPOTIFY_METHOD2(SpotifyPlaylist, __construct, &temp, return_value, object, playlist);
}

static int has_entity_loaded;

static void entity_loaded()
{
	has_entity_loaded = 1;
}

PHP_METHOD(Spotify, getPlaylistByURI)
{
	zval *uri, temp, *object = getThis();
	int timeout = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &uri) == FAILURE) {
		return;
	}

	spotify_object *p = (spotify_object*)zend_object_store_get_object(object TSRMLS_CC);

	sp_link *link = sp_link_create_from_string(Z_STRVAL_P(uri));
	if (NULL == link) {
		RETURN_FALSE;
	}

	if (SP_LINKTYPE_PLAYLIST != sp_link_type(link)) {
		RETURN_FALSE;
	}

	sp_playlist *playlist = sp_playlist_create(p->session, link);
	if (NULL == playlist) {
		RETURN_FALSE;
	}

	while (!sp_playlist_is_loaded(playlist)) {	
		metadata_updated_fn = entity_loaded;
		do {
			sp_session_process_events(p->session, &timeout);
		} while (has_entity_loaded != 1 || timeout == 0);
		has_entity_loaded = 0;
		metadata_updated_fn = NULL;
	}

	object_init_ex(return_value, spotifyplaylist_ce);
	SPOTIFY_METHOD2(SpotifyPlaylist, __construct, &temp, return_value, object, playlist);
}

PHP_METHOD(Spotify, getTrackByURI)
{
	zval *uri, temp, *object = getThis();
	int timeout = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &uri) == FAILURE) {
		return;
	}

	spotify_object *p = (spotify_object*)zend_object_store_get_object(object TSRMLS_CC);

	sp_link *link = sp_link_create_from_string(Z_STRVAL_P(uri));
	if (NULL == link) {
		RETURN_FALSE;		
	}

	if (SP_LINKTYPE_TRACK != sp_link_type(link)) {
		RETURN_FALSE;
	}

	sp_track *track = sp_link_as_track(link);

	while (!sp_track_is_loaded(track)) {
		metadata_updated_fn = entity_loaded;
		do {
			sp_session_process_events(p->session, &timeout);
		} while (has_entity_loaded != 1 || timeout == 0);
		has_entity_loaded = 0;
		metadata_updated_fn = NULL;
	}
	
	object_init_ex(return_value, spotifytrack_ce);
	SPOTIFY_METHOD2(SpotifyTrack, __construct, &temp, return_value, object, track);
}

PHP_METHOD(Spotify, getAlbumByURI)
{
	zval *uri, temp, *object = getThis();
	int timeout = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &uri) == FAILURE) {
		return;
	}

	spotify_object *p = (spotify_object*)zend_object_store_get_object(object TSRMLS_CC);

	sp_link *link = sp_link_create_from_string(Z_STRVAL_P(uri));
	if (NULL == link) {
		RETURN_FALSE;
	}

	if (SP_LINKTYPE_ALBUM != sp_link_type(link)) {
		RETURN_FALSE;
	}

	sp_album *album = sp_link_as_album(link);

	while (!sp_album_is_loaded(album)) {
		metadata_updated_fn = entity_loaded;
		do {
			sp_session_process_events(p->session, &timeout);
		} while (has_entity_loaded != 1 || timeout == 0);
		has_entity_loaded = 0;
		metadata_updated_fn = NULL;
	}

	object_init_ex(return_value, spotifyalbum_ce);
	SPOTIFY_METHOD2(SpotifyAlbum, __construct, &temp, return_value, object, album);
}

static sp_playlistcontainer_callbacks playlistcontainer_callbacks = {
	NULL,
	NULL,
	NULL,
	playlistcontainer_complete
};

static void playlistcontainer_complete(sp_playlistcontainer *pc, void *userdata)
{
	spotify_object *p = (spotify_object*)userdata;
	p->playlistcontainer = pc;
	sp_playlistcontainer_remove_callbacks(pc, &playlistcontainer_callbacks, userdata);
}

PHP_METHOD(Spotify, initPlaylistContainer)
{
	int timeout = 0;

	spotify_object *p = (spotify_object*)zend_object_store_get_object(getThis() TSRMLS_CC);
	if (p->playlistcontainer != NULL) {
		RETURN_TRUE;
	}

	sp_playlistcontainer *tempcontainer = sp_session_playlistcontainer(p->session);
	sp_playlistcontainer_add_callbacks(tempcontainer, &playlistcontainer_callbacks, p);

	while (p->playlistcontainer == NULL) {
		sp_session_process_events(p->session, &timeout);
	}

	RETURN_TRUE;
}

static void logged_in(sp_session *session, sp_error error)
{
	is_logged_in = 1;

	if (SP_ERROR_OK != error) {
		is_logged_out = 1;

		char *errMsg;
		spprintf(&errMsg, 0, "login failed: %s", sp_error_message(error));
		sp_session_release(session);
		zend_throw_exception((zend_class_entry*)zend_exception_get_default(), errMsg, 0 TSRMLS_CC);
	}
}

static void logged_out(sp_session *session)
{
	is_logged_out = 1;
}

static void log_message(sp_session *session, const char *data)
{
	//php_printf("SPOTIFY_ERR: %s\n", data);
}

function_entry spotify_methods[] = {
	PHP_ME(Spotify, __construct,            NULL,   ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(Spotify, __destruct,				NULL,	ZEND_ACC_PUBLIC|ZEND_ACC_DTOR)
	PHP_ME(Spotify, getPlaylists,			NULL,	ZEND_ACC_PUBLIC)
	PHP_ME(Spotify, getStarredPlaylist,     NULL,   ZEND_ACC_PUBLIC)
	PHP_ME(Spotify, getPlaylistByURI,		NULL,	ZEND_ACC_PUBLIC)
	PHP_ME(Spotify, getTrackByURI,			NULL,	ZEND_ACC_PUBLIC)
	PHP_ME(Spotify, getAlbumByURI,			NULL,	ZEND_ACC_PUBLIC)
	PHP_ME(Spotify, initPlaylistContainer,	NULL,	ZEND_ACC_PRIVATE)
	{NULL, NULL, NULL}
};

void spotify_free_storage(void *object TSRMLS_DC)
{
	spotify_object *obj = (spotify_object*)object;
	zend_hash_destroy(obj->std.properties);
	FREE_HASHTABLE(obj->std.properties);
	efree(obj);
}

zend_object_value spotify_create_handler(zend_class_entry *type TSRMLS_DC)
{
	zval *tmp;
	zend_object_value retval;

	spotify_object *obj = (spotify_object *)emalloc(sizeof(spotify_object));
	memset(obj, 0, sizeof(spotify_object));
   // obj->std.ce = type;

	zend_object_std_init(&obj->std, type TSRMLS_CC);
    zend_hash_copy(obj->std.properties, &type->default_properties,
        (copy_ctor_func_t)zval_add_ref, (void *)&tmp, sizeof(zval *));

    retval.handle = zend_objects_store_put(obj, NULL,
        spotify_free_storage, NULL TSRMLS_CC);
    retval.handlers = &spotify_object_handlers;

    return retval;
}

PHP_MINIT_FUNCTION(spotify)
{
	zend_class_entry ce;
	INIT_CLASS_ENTRY(ce, "Spotify", spotify_methods);
	spotify_ce = zend_register_internal_class(&ce TSRMLS_CC);
	spotify_ce->create_object = spotify_create_handler;
	memcpy(&spotify_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	spotify_object_handlers.clone_obj = NULL;

	spotify_init_playlist(TSRMLS_C);
	spotify_init_track(TSRMLS_C);
	spotify_init_artist(TSRMLS_C);
	spotify_init_album(TSRMLS_C);
	spotify_init_user(TSRMLS_C);

	return SUCCESS;
}

zend_module_entry spotify_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
    STANDARD_MODULE_HEADER,
#endif
    PHP_SPOTIFY_EXTNAME,
    NULL,        /* Functions */
    PHP_MINIT(spotify),        /* MINIT */
    NULL,        /* MSHUTDOWN */
    NULL,        /* RINIT */
    NULL,        /* RSHUTDOWN */
    NULL,        /* MINFO */
#if ZEND_MODULE_API_NO >= 20010901
    PHP_SPOTIFY_VERSION,
#endif
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_SPOTIFY
ZEND_GET_MODULE(spotify)
#endif
