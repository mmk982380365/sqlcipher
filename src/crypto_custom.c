
#ifdef SQLITE_HAS_CODEC

#include "sqliteInt.h"
#include "sqlcipher.h"

/* Structure to store registered custom providers, in a list */
typedef struct sqlcipher_named_provider {
    sqlcipher_provider p;
    char name[0];   /* dynamic length */
} sqlcipher_named_provider;

/* Provider context */
typedef struct custom_ctx {
    sqlcipher_provider *p;  /* Currently selected provider */
    void *p_ctx;            /* Context for selected provider */
} custom_ctx;


/* Custom provider list */
static sqlcipher_named_provider **custom_providers;
static int custom_providers_count;
static int custom_providers_capacity;
static sqlite3_mutex *custom_providers_mutex;

static sqlcipher_provider *fallback_provider;    /* Default fallback provider, should be openssl, libtomcrypt or commoncrypto */
static int activate_count = 0;

static void provider_overload(const sqlcipher_provider *base, sqlcipher_provider *p) {
    /* sqlcipher_provider is actually a pile of function pointers, which has the same size of (void *).
       We can just run a loop comparing and assigning raw pointers. */
    int n = sizeof(sqlcipher_provider) / sizeof(void *);
    int i;
    for (i = 0; i < n; i++) {
        if ( ((void **) p)[i] == 0 )
            ((void **) p)[i] = ((void **) base)[i];
    }
}

static int select_provider(custom_ctx *ctx, const char *name) {
    int rc = SQLITE_OK;
    
    sqlite3_mutex_enter(custom_providers_mutex);
    if (ctx->p) goto end;

    /* Select provider according to name. */
    if (name) {
        int i;
        for (i = 0; i < custom_providers_count; i++) {
            if (strcmp(custom_providers[i]->name, name) == 0) {
                ctx->p = &custom_providers[i]->p;
                break;
            }
        }
    }

    if (!ctx->p)
        ctx->p = fallback_provider;

    /* Now we have chosen which provider will be used, 
       we can initialize the real provider context. */
    ctx->p_ctx = NULL;
    rc = ctx->p->ctx_init(&ctx->p_ctx);
    
end:
    sqlite3_mutex_leave(custom_providers_mutex);
    return rc;
}

int sqlcipher_register_custom_provider(const char *name, const sqlcipher_provider *p) {
    sqlite3_mutex_enter(custom_providers_mutex);

    /* Grow custom provider list if it's full. */
    if (custom_providers_count >= custom_providers_capacity) {
        /* Linear growth should be enough here as we probably don't have so much providers. */
        int new_capacity = custom_providers_capacity + 16;
        void *new_list = sqlite3_realloc(custom_providers, new_capacity * sizeof(sqlcipher_named_provider *));
        if (!new_list) goto bail;

        custom_providers = (sqlcipher_named_provider **) new_list;
        custom_providers_capacity = new_capacity;
    }
    
    size_t len = strlen(name) + 1;
    sqlcipher_named_provider *np = sqlite3_malloc(sizeof(sqlcipher_named_provider) + len * sizeof(char));
    if (!np) goto bail;

    /* Overload provider functions. */
    strncpy(np->name, name, len);
    memcpy(&np->p, p, sizeof(sqlcipher_provider));
    provider_overload(fallback_provider, &np->p);

    /* Find existing provider. */
    int i;
    for (i = 0; i < custom_providers_count; i++) {
        if (strcmp(custom_providers[i]->name, name) == 0)
            break;
    }
    if (i < custom_providers_count) {
        /* Free previous provider and replace with new one. */
        sqlite3_free(custom_providers[i]);
    } else {
        /* Not found, enlarge the list. */
        custom_providers_count++;
    }
    custom_providers[i] = np;

    sqlite3_mutex_leave(custom_providers_mutex);
    return SQLITE_OK;

bail:
    sqlite3_mutex_leave(custom_providers_mutex);
    return SQLITE_NOMEM;
}

int sqlcipher_unregister_custom_provider(const char *name) {
    sqlite3_mutex_enter(custom_providers_mutex);

    /* Find existing provider. */
    int i;
    for (i = 0; i < custom_providers_count; i++) {
        if (strcmp(custom_providers[i]->name, name) == 0)
            break;
    }
    if (i < custom_providers_count) {
        /* Found, free the provider and swap it to the end of the list. */
        sqlite3_free(custom_providers[i]);
        custom_providers_count--;
        custom_providers[i] = custom_providers[custom_providers_count];
    }

    sqlite3_mutex_leave(custom_providers_mutex);
    return SQLITE_OK;
}

static int sqlcipher_custom_activate(void *ctx) {
    sqlite3_mutex *mutex = sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER);
    sqlite3_mutex_enter(mutex);

    if (!fallback_provider) {
        fallback_provider = (sqlcipher_provider *) malloc(sizeof(sqlcipher_provider));
        if (!fallback_provider) goto bail;

#if defined (SQLCIPHER_CRYPTO_CC)
        extern int sqlcipher_cc_setup(sqlcipher_provider *p);
        sqlcipher_cc_setup(fallback_provider);
#elif defined (SQLCIPHER_CRYPTO_LIBTOMCRYPT)
        extern int sqlcipher_ltc_setup(sqlcipher_provider *p);
        sqlcipher_ltc_setup(fallback_provider);
#elif defined (SQLCIPHER_CRYPTO_OPENSSL)
        extern int sqlcipher_openssl_setup(sqlcipher_provider *p);
        sqlcipher_openssl_setup(fallback_provider);
#else
#error "NO DEFAULT SQLCIPHER CRYPTO PROVIDER DEFINED"
#endif

        custom_providers_mutex = sqlite3_mutex_alloc(SQLITE_MUTEX_FAST);
    }

    activate_count++;
    sqlite3_mutex_leave(mutex);
    return SQLITE_OK;

bail:
    sqlite3_mutex_leave(mutex);
    return SQLITE_NOMEM;
}

static int sqlcipher_custom_deactivate(void *ctx) {
    sqlite3_mutex *mutex = sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_MASTER);
    sqlite3_mutex_enter(mutex);

    if (--activate_count == 0) {
        sqlite3_free(fallback_provider);
        fallback_provider = NULL;
        sqlite3_mutex_free(custom_providers_mutex);
    }

    sqlite3_mutex_leave(mutex);
    return SQLITE_OK;
}

static int sqlcipher_custom_ctx_init(void **ctx) {
    custom_ctx *c;
    *ctx = c = sqlite3_malloc(sizeof(sizeof(custom_ctx)));
    if (!c) return SQLITE_NOMEM;

    sqlcipher_custom_activate(c);
    c->p = NULL;
    c->p_ctx = NULL;
    return SQLITE_OK;
}

static int sqlcipher_custom_ctx_free(void **ctx) {
    sqlcipher_custom_deactivate(*ctx);
    sqlite3_free(*ctx);
    return SQLITE_OK;
}

static int sqlcipher_custom_ctx_copy(void *target_ctx, void *source_ctx) {
    memcpy(target_ctx, source_ctx, sizeof(custom_ctx));
    return SQLITE_OK;
}

static int sqlcipher_custom_ctx_cmp(void *c1, void *c2) {
    custom_ctx *ctx1 = (custom_ctx *) c1;
    custom_ctx *ctx2 = (custom_ctx *) c2;
    if (ctx1->p != ctx2->p) return 0;
    if (ctx1->p_ctx && ctx2->p_ctx)
        return ctx1->p->ctx_cmp(ctx1->p_ctx, ctx2->p_ctx);
    if (!ctx1->p_ctx && !ctx2->p_ctx)
        return 1;
    return 0;
}

static const char* sqlcipher_custom_get_provider_name(void *ctx) {
    return "custom";
}

static const char* sqlcipher_custom_get_provider_version(void *ctx) {
    return "0.2.2";
}

static int sqlcipher_custom_set_cipher(void *ctx_, const char *cipher_name) {
    custom_ctx *ctx = (custom_ctx *) ctx_;
    int rc;
    /* Initialize provider accroding to cipher_name. */
    if (!ctx->p && (rc = select_provider(ctx, cipher_name)) != SQLITE_OK) 
        return rc;
    return ctx->p->set_cipher(ctx->p_ctx, cipher_name);
}

static const char* sqlcipher_custom_get_cipher(void *ctx_) {
    custom_ctx *ctx = (custom_ctx *) ctx_;
    if (!ctx->p && select_provider(ctx, NULL) != SQLITE_OK) 
        return "";
    return ctx->p->get_cipher(ctx->p_ctx);
}

static int sqlcipher_custom_random(void *ctx_, void *buffer, int length) {
    custom_ctx *ctx = (custom_ctx *) ctx_;
    int rc;
    if (!ctx->p && (rc = select_provider(ctx, NULL)) != SQLITE_OK) 
        return rc;
    return ctx->p->random(ctx->p_ctx, buffer, length);
}

static int sqlcipher_custom_add_random(void *ctx_, void *buffer, int length) {
    custom_ctx *ctx = (custom_ctx *) ctx_;
    int rc;
    if (!ctx->p && (rc = select_provider(ctx, NULL)) != SQLITE_OK) 
        return rc;
    return ctx->p->add_random(ctx->p_ctx, buffer, length);
}

static int sqlcipher_custom_hmac(void *ctx_, unsigned char *hmac_key, int key_sz, 
        unsigned char *in, int in_sz, unsigned char *in2, int in2_sz, unsigned char *out) {
    custom_ctx *ctx = (custom_ctx *) ctx_;
    int rc;
    if (!ctx->p && (rc = select_provider(ctx, NULL)) != SQLITE_OK) 
        return rc;
    return ctx->p->hmac(ctx->p_ctx, hmac_key, key_sz, in, in_sz, in2, in2_sz, out);
}

static int sqlcipher_custom_kdf(void *ctx_, const unsigned char *pass, int pass_sz, 
        unsigned char* salt, int salt_sz, int workfactor, int key_sz, unsigned char *key) {
    custom_ctx *ctx = (custom_ctx *) ctx_;
    int rc;
    if (!ctx->p && (rc = select_provider(ctx, NULL)) != SQLITE_OK) 
        return rc;
    return ctx->p->kdf(ctx->p_ctx, pass, pass_sz, salt, salt_sz, workfactor, key_sz, key);
}

static int sqlcipher_custom_cipher(void *ctx_, int mode, unsigned char *key, int key_sz, 
        unsigned char *iv, unsigned char *in, int in_sz, unsigned char *out) {
    custom_ctx *ctx = (custom_ctx *) ctx_;
    int rc;
    if (!ctx->p && (rc = select_provider(ctx, NULL)) != SQLITE_OK) 
        return rc;
    return ctx->p->cipher(ctx->p_ctx, mode, key, key_sz, iv, in, in_sz, out);
}

static int sqlcipher_custom_get_key_sz(void *ctx_) {
    custom_ctx *ctx = (custom_ctx *) ctx_;
    if (!ctx->p && select_provider(ctx, NULL) != SQLITE_OK) 
        return 0;
    return ctx->p->get_key_sz(ctx->p_ctx);
}

static int sqlcipher_custom_get_iv_sz(void *ctx_) {
    custom_ctx *ctx = (custom_ctx *) ctx_;
    if (!ctx->p && select_provider(ctx, NULL) != SQLITE_OK) 
        return 0;
    return ctx->p->get_iv_sz(ctx->p_ctx);
}

static int sqlcipher_custom_get_block_sz(void *ctx_) {
    custom_ctx *ctx = (custom_ctx *) ctx_;
    if (!ctx->p && select_provider(ctx, NULL) != SQLITE_OK) 
        return 0;
    return ctx->p->get_block_sz(ctx->p_ctx);
}

static int sqlcipher_custom_get_hmac_sz(void *ctx_) {
    custom_ctx *ctx = (custom_ctx *) ctx_;
    if (!ctx->p && select_provider(ctx, NULL) != SQLITE_OK) 
        return 0;
    return ctx->p->get_hmac_sz(ctx->p_ctx);
}

static int sqlcipher_custom_fips_status(void *ctx_) {
    custom_ctx *ctx = (custom_ctx *) ctx_;
    if (!ctx->p && select_provider(ctx, NULL) != SQLITE_OK) 
        return 0;
    return ctx->p->fips_status(ctx->p_ctx);
}


int sqlcipher_custom_setup(sqlcipher_provider *p) {
    p->activate = sqlcipher_custom_activate;  
    p->deactivate = sqlcipher_custom_deactivate;
    p->get_provider_name = sqlcipher_custom_get_provider_name;
    p->random = sqlcipher_custom_random;
    p->hmac = sqlcipher_custom_hmac;
    p->kdf = sqlcipher_custom_kdf;
    p->cipher = sqlcipher_custom_cipher;
    p->set_cipher = sqlcipher_custom_set_cipher;
    p->get_cipher = sqlcipher_custom_get_cipher;
    p->get_key_sz = sqlcipher_custom_get_key_sz;
    p->get_iv_sz = sqlcipher_custom_get_iv_sz;
    p->get_block_sz = sqlcipher_custom_get_block_sz;
    p->get_hmac_sz = sqlcipher_custom_get_hmac_sz;
    p->ctx_copy = sqlcipher_custom_ctx_copy;
    p->ctx_cmp = sqlcipher_custom_ctx_cmp;
    p->ctx_init = sqlcipher_custom_ctx_init;
    p->ctx_free = sqlcipher_custom_ctx_free;
    p->add_random = sqlcipher_custom_add_random;
    p->fips_status = sqlcipher_custom_fips_status;
    p->get_provider_version = sqlcipher_custom_get_provider_version;
    return SQLITE_OK;
}

#endif /* SQLITE_HAS_CODEC */