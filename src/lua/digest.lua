-- digest.lua (internal file)

local ffi = require 'ffi'
local buffer = require('buffer')

ffi.cdef[[
    /* internal implementation */
    unsigned char *SHA1internal(const unsigned char *d, size_t n, unsigned char *md);

    /* from libc */
    int snprintf(char *str, size_t size, const char *format, ...);

    typedef uint32_t (*crc32_func)(uint32_t crc,
        const unsigned char *buf, unsigned int len);
    extern int32_t guava(int64_t state, int32_t buckets);
    extern crc32_func crc32_calc;

    /* base64 */
    int base64_bufsize(int binsize);
    int base64_decode(const char *in_base64, int in_len, char *out_bin, int out_len);
    int base64_encode(const char *in_bin, int in_len, char *out_base64, int out_len);

    /* random */
    void random_bytes(char *, size_t);

    /* from third_party/PMurHash.h */
    void PMurHash32_Process(uint32_t *ph1, uint32_t *pcarry, const void *key, int len);
    uint32_t PMurHash32_Result(uint32_t h1, uint32_t carry, uint32_t total_length);
    uint32_t PMurHash32(uint32_t seed, const void *key, int len);

    /* from openssl/evp.h */
    void OpenSSL_add_all_digests();
    void OpenSSL_add_all_ciphers();
    typedef void ENGINE;

    typedef struct {} EVP_MD_CTX;
    typedef struct {} EVP_MD;
    EVP_MD_CTX *EVP_MD_CTX_create(void);
    void EVP_MD_CTX_destroy(EVP_MD_CTX *ctx);
    int EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *type, ENGINE *impl);
    int EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt);
    int EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *s);
    const EVP_MD *EVP_get_digestbyname(const char *name);

    typedef struct {} EVP_CIPHER_CTX;
    typedef struct {} EVP_CIPHER;
    EVP_CIPHER_CTX *EVP_CIPHER_CTX_new();
    void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *ctx);

    int EVP_CipherInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *cipher,
                          ENGINE *impl, const unsigned char *key,
                          const unsigned char *iv, int enc);
    int EVP_CipherUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl,
                     const unsigned char *in, int inl);
    int EVP_CipherFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl);
    int EVP_CIPHER_CTX_cleanup(EVP_CIPHER_CTX *ctx);

    int EVP_CIPHER_block_size(const EVP_CIPHER *cipher);
    const EVP_CIPHER *EVP_get_cipherbyname(const char *name);
]]

local ssl
if ssl == nil then
    local variants = {
        'ssl.so.10',
        'ssl.so.1.0.0',
        'ssl.so.0.9.8',
        'ssl.so',
        'ssl',
    }

    for _, libname in pairs(variants) do
        pcall(function() ssl = ffi.load(libname) end)
        if ssl ~= nil then
            ssl.OpenSSL_add_all_digests()
            ssl.OpenSSL_add_all_ciphers()
            break
        end
    end
end

local digest_shortcuts = {
    sha     = 'SHA',
    sha224  = 'SHA224',
    sha256  = 'SHA256',
    sha384  = 'SHA384',
    sha512  = 'SHA512',
    md5     = 'MD5',
    md4     = 'MD4',
}

local hexres = ffi.new('char[129]')

local function str_to_hex(r)
    for i = 0, r:len() - 1 do
        ffi.C.snprintf(hexres + i * 2, 3, "%02x",
            ffi.cast('unsigned char', r:byte(i + 1)))
    end
    return ffi.string(hexres, r:len() * 2)
end

local PMurHash
local PMurHash_methods = {

    update = function(self, str)
        str = tostring(str or '')
        ffi.C.PMurHash32_Process(self.seed, self.value, str, string.len(str))
        self.total_length = self.total_length + string.len(str)
    end,

    result = function(self)
        return ffi.C.PMurHash32_Result(self.seed[0], self.value[0], self.total_length)
    end,

    clear = function(self)
        self.seed[0] = self.default_seed
        self.total_length = 0
        self.value[0] = 0
    end,

    copy = function(self)
        local new_self = PMurHash.new()
        new_self.seed[0] = self.seed[0]
        new_self.value[0] = self.value[0]
        new_self.total_length = self.total_length
        return new_self
    end
}

PMurHash = {
    default_seed = 13,

    new = function(opts)
        opts = opts or {}
        local self = setmetatable({}, { __index = PMurHash_methods })
        self.default_seed = (opts.seed or PMurHash.default_seed)
        self.seed = ffi.new("int[1]", self.default_seed)
        self.value = ffi.new("int[1]", 0)
        self.total_length = 0
        return self
    end
}

setmetatable(PMurHash, {
    __call = function(self, str)
        str = tostring(str or '')
        return ffi.C.PMurHash32(PMurHash.default_seed, str, string.len(str))
    end
})

local CRC32
local CRC32_methods = {
    update = function(self, str)
        str = tostring(str or '')
        self.value = ffi.C.crc32_calc(self.value, str, string.len(str))
    end,

    result = function(self)
        return self.value
    end,

    clear = function(self)
        self.value = CRC32.crc_begin
    end,

    copy = function(self)
        local new_self = CRC32.new()
        new_self.value = self.value
        return new_self
    end
}

CRC32 = {
    crc_begin = 4294967295,

    new = function()
        local self = setmetatable({}, { __index = CRC32_methods })
        self.value = CRC32.crc_begin
        return self
    end
}

setmetatable(CRC32, {
    __call = function(self, str)
        str = tostring(str or '')
        return ffi.C.crc32_calc(CRC32.crc_begin, str, string.len(str))
    end
})

local function digest_gc(ctx)
    ssl.EVP_MD_CTX_destroy(ctx)
end

local function digest_new()
    local ctx = ssl.EVP_MD_CTX_create()
    if ctx == nil then
        return error('Can\'t create digest ctx')
    end
    ffi.gc(ctx, digest_gc)
    local self = setmetatable({}, digest_mt)
    self.ctx = ctx
    self.buf = buffer.ibuf(64)
    self.initialized = false
    self.outl = ffi.new('int[1]')
    return self
end

local function digest_init(ctx, name)
    local digest = ssl.EVP_get_digestbyname(name);
    if digest == nil then
        return error('Can\'t find digest ' .. name)
    end
    if ssl.EVP_DigestInit_ex(ctx.ctx, digest, nil) ~= 1 then
        return error('Can\'t init digest')
    end
    ctx.initialized = true
end

local function digest_update(ctx, input)
    if not ctx.initialized then
        return error('Digest not initialized')
    end
    if ssl.EVP_DigestUpdate(ctx.ctx, input, input:len()) ~= 1 then
        return error('Can\'t update digest')
    end
end

local function digest_final(ctx)
    if not ctx.initialized then
        return error('Digest not initialized')
    end
    if ssl.EVP_DigestFinal_ex(ctx.ctx, ctx.buf.wpos, ctx.outl) ~= 1 then
        return error('Can\'t final digest')
    end
    return ffi.string(ctx.buf.wpos, ctx.outl[0])
end

local function digest_free(ctx)
    ssl.EVP_MD_CTX_destroy(ctx.ctx)
    ffi.gc(ctx.ctx, nil)
end

digest_mt = {
    __index = {
          init = digest_init,
          update = digest_update,
          final = digest_final,
          free = digest_free
    }
}

local function cipher_gc(ctx)
    ssl.EVP_CIPHER_CTX_free(ctx)
end

local function cipher_new(enc)
    local ctx = ssl.EVP_CIPHER_CTX_new()
    if ctx == nil then
        return error('Can\'t create cipher ctx')
    end
    ffi.gc(ctx, cipher_gc) 
    local self = setmetatable({}, cipher_mt)
    self.ctx = ctx
    self.enc = enc
    self.buf = buffer.ibuf()
    self.block_size = nil
    self.initialized = false
    self.outl = ffi.new('int[1]')
    return self
end

local function encrypt_new(name)
    return cipher_new(1)
end

local function decrypt_new(name)
    return cipher_new(0)
end

local function cipher_init(ctx, name, key, iv)
    local  ciph = ssl.EVP_get_cipherbyname(name)
    if ciph == nil then
        return error('Can\'t find cipher ' .. name)
    end
    ctx.block_size = ssl.EVP_CIPHER_block_size(ciph)
    if ssl.EVP_CipherInit_ex(ctx.ctx, ciph, nil, key, iv, ctx.enc) ~= 1 then
        return error('Can\'t init cipher')
    end
    ctx.initialized = true
end

local function cipher_update(ctx, input)
    if not ctx.initialized then
        return error('Cipher not initialized')
    end
    local wpos = ctx.buf:reserve(input:len() + ctx.block_size - 1)
    if ssl.EVP_CipherUpdate(ctx.ctx, wpos, ctx.outl, input, input:len()) ~= 1 then
        return error('Can\'t update cipher')
    end
    return ffi.string(wpos, ctx.outl[0])
end

local function cipher_final(ctx)
    if not ctx.initialized then
        return error('Cipher not initialized')
    end
    local wpos = ctx.buf:reserve(ctx.block_size - 1)
    if ssl.EVP_CipherFinal_ex(ctx.ctx, wpos, ctx.outl) ~= 1 then
        return error('Can\'t final cipher')
    end
    ctx.initialized = false
    return ffi.string(wpos, ctx.outl[0])
end

local function cipher_free(ctx)
    ssl.EVP_CIPHER_CTX_free(ctx.ctx)
    ffi.gc(ctx.ctx, nil)
    ctx.buf:reset()
end

cipher_mt = {
    __index = {
          init = cipher_init,
          update = cipher_update,
          final = cipher_final,
          free = cipher_free
    }
}


local m = {
    base64_encode = function(bin)
        if type(bin) ~= 'string' then
            error('Usage: digest.base64_encode(string)')
        end
        local blen = #bin
        local slen = ffi.C.base64_bufsize(blen)
        local str  = ffi.new('char[?]', slen)
        local len = ffi.C.base64_encode(bin, blen, str, slen)
        return ffi.string(str, len)
    end,

    base64_decode = function(str)
        if type(str) ~= 'string' then
            error('Usage: digest.base64_decode(string)')
        end
        local slen = #str
        local blen = math.ceil(slen * 3 / 4)
        local bin  = ffi.new('char[?]', blen)
        local len = ffi.C.base64_decode(str, slen, bin, blen)
        return ffi.string(bin, len)
    end,

    crc32 = CRC32,

    crc32_update = function(crc, str)
        str = tostring(str or '')
        return ffi.C.crc32_calc(tonumber(crc), str, string.len(str))
    end,

    sha1 = function(str)
        if str == nil then
            str = ''
        else
            str = tostring(str)
        end
        local r = ffi.C.SHA1internal(str, #str, nil)
        return ffi.string(r, 20)
    end,

    sha1_hex = function(str)
        if str == nil then
            str = ''
        else
            str = tostring(str)
        end
        local r = ffi.C.SHA1internal(str, #str, nil)
        return str_to_hex(ffi.string(r, 20))
    end,

    guava = function(state, buckets)
       return ffi.C.guava(state, buckets)
    end,

    urandom = function(n)
        if n == nil then
            error('Usage: digest.urandom(len)')
        end
        local buf = ffi.new('char[?]', n)
        ffi.C.random_bytes(buf, n)
        return ffi.string(buf, n)
    end,

    murmur = PMurHash,

    digest_ctx = digest_new,
    digest = function (name, str) 
        local ctx = digest_new()
        ctx:init(name)
        ctx:update(str)
        local res = ctx:final()
        ctx:free()
        return res
    end,
    encrypt_ctx = encrypt_new,
    encrypt = function(name, str, key, iv)
        local ctx = encrypt_new()
        ctx:init(name, key, iv)
        local res = ctx:update(str) .. ctx:final()
        ctx:free()
        return res
    end,
    decrypt_ctx = decrypt_new,
    decrypt = function(name, str, key, iv)
        local ctx = decrypt_new()
        ctx:init(name, key, iv)
        local res = ctx:update(str) .. ctx:final()
        ctx:free()
        return res
    end
}

if ssl ~= nil then
    for pname, hfunction in pairs(digest_shortcuts) do

        m[ pname ] = function(str)
            if str == nil then
                str = ''
            else
                str = tostring(str)
            end
            return m.digest(hfunction, str)
        end

        m[ pname .. '_hex' ] = function(str)
            if str == nil then
                str = ''
            else
                str = tostring(str)
            end
            local r = m.digest(hfunction, str)
            return str_to_hex(r)
        end
    end
else
    local function errorf()
        error("libSSL was not loaded")
    end
    for pname, df in pairs(def) do
        m[ pname ] = errorf
        m[ pname .. '_hex' ] = errorf
    end
end

return m
