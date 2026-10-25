/* Production write-back regression: fake-shared pages must not discard a terrain normal map; the CPU's own changes
   must still win. BotW additionally exercises the tiny-target completion path on real GPU clears, without an invalidate.
   No game data. Included after the production helpers, as the sampler-border and texel-region probes are. */
int mtl_probe_writeback(void) {
    int checked = 0;
    wb_ent *entry = calloc(1, sizeof *entry);
    uint8_t *memory = calloc(1, 512 * 1024);
    if (!entry || !memory) MDIE("writeback probe allocation");
    entry->t.mem = memory; entry->t.L.size = 214528;   // independently sized 260x260 RG8 mip chain
    wb_snap(entry);
    if (!entry->snap || entry->snap_n != entry->t.L.size) MDIE("terrain writeback probe: no exact snapshot");
    checked++;
    memory[entry->snap_n + 7] = 42;
    if (memcmp(entry->snap, memory, entry->snap_n)) MDIE("writeback probe: neighbour affected snapshot");
    checked++;
    memory[entry->snap_n - 1] = 23;
    if (!memcmp(entry->snap, memory, entry->snap_n)) MDIE("writeback probe: missed CPU write at end of texture");
    checked++;
    wb_snap(entry);
    if (memcmp(entry->snap, memory, entry->snap_n)) MDIE("writeback probe: repeated render kept an old snapshot");
    checked++;
    entry->t.L.size = WB_SNAP_MAX + 1;
    wb_snap(entry);
    if (entry->snap || entry->snap_n) MDIE("writeback probe: oversized target kept a snapshot");
    checked++;
    wb_ent_free(entry); free(memory);
#ifdef GAME_BOTW
    mtl_init();
    tex_t t = {.serial = 0xffffffa9, .d = {.width = 1, .height = 1, .depth = 1, .levels = 1,
        .target = 1, .format = NVN_FORMAT_RGBA32F, .flags = 0x40, .swizzle_r = 2, .swizzle_g = 3, .swizzle_b = 4, .swizzle_a = 5}};
    nvn_tex_layout(&t.d, &t.L);
    size_t bytes = (t.L.size + getpagesize() - 1) & ~((size_t)getpagesize() - 1);
    uint8_t *mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) MDIE("tiny writeback probe mmap");
    pool_t pool = {.mem = mem, .size = bytes, .flags = 0x24}; t.mem = mem; t.pool = &pool;
    mtl_pool_add(mem, bytes); nvn_dirty_add_range(mem, bytes);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:d]; if (!target) MDIE("tiny writeback probe texture"); res_add(target);
    for (int scenario = 0; scenario < 3; scenario++) {
        dm_guest_write_prepare(mem, bytes); memset(mem, 0, bytes);
        mtl_sub_begin(); op_hdr end = {OP_END, 8}; mtl_exec((const uint8_t *)&end);
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        rp.colorAttachments[0].texture = target; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(.25, .5, .75, 1);
        rp.renderTargetWidth = rp.renderTargetHeight = 1;
        id<MTLRenderCommandEncoder> en = enc_r_begin(rp); enc_r_end(en);
        botw_tiny_note(&t, (__bridge const void *)target);
        if (scenario == 1) { dm_guest_write_prepare(mem, bytes); ((float *)mem)[0] = .875f; }
        if (scenario == 2) botw_tiny_note(&t, (__bridge const void *)target);   // re-rendering the same serial is coalesced
        mtl_sub_commit(); mtl_finish();
        const float want[4] = {.25f, .5f, .75f, 1};
        for (int c = 0; c < 4; c++) {
            float expected = scenario == 1 ? (c == 0 ? .875f : 0) : want[c];
            if (((float *)mem)[c] != expected) MDIE("tiny writeback probe scenario %d channel %d: %g != %g", scenario, c, ((float *)mem)[c], expected);
            checked++;
        }
        for (size_t i = 16; i < bytes; i++) { if (mem[i]) MDIE("tiny writeback probe wrote padding"); checked++; }
    }
    res_remove(target); nvn_dirty_remove_range(mem); mtl_pool_del(mem); munmap(mem, bytes);

    /* Exercise the public flush entry with the two CPU-read grass-mask layouts. A tiny requested range must make
       its rendered texture coherent; empty/unrelated ranges must not, and a later CPU write must still win. */
    extern void *nvn_slot_fn(const char *name);
    void (*flush)(const NVNmemoryPool *, ptrdiff_t, size_t) = nvn_slot_fn("nvnMemoryPoolFlushMappedRange");
    for (int layout = 0; layout < 2; layout++) {
        unsigned side = layout ? 512 : 256, bpp = layout ? 4 : 2;
        tex_t mask = {.serial = 0xffffffaa + layout, .d = {.width = side, .height = side, .depth = 1, .levels = 1,
            .target = 1, .format = layout ? NVN_FORMAT_RGBA8 : NVN_FORMAT_RG8, .flags = 0x40,
            .swizzle_r = 2, .swizzle_g = 3, .swizzle_b = 4, .swizzle_a = 5}};
        nvn_tex_layout(&mask.d, &mask.L);
        size_t span = (mask.L.size + getpagesize() * 2 - 1) & ~((size_t)getpagesize() - 1);
        uint8_t *backing = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (backing == MAP_FAILED) MDIE("mapped flush probe mmap");
        pool_t cached = {.magic = MAGIC('P','O','O','L'), .mem = backing, .size = span, .flags = 0x24};
        mask.mem = backing; mask.pool = &cached;
        mtl_pool_add(backing, span); nvn_dirty_add_range(backing, span);
        MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:layout ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRG8Unorm width:side height:side mipmapped:NO];
        md.storageMode = MTLStorageModePrivate; md.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> rendered = [g_dev newTextureWithDescriptor:md];
        if (!rendered) MDIE("mapped flush probe texture");
        res_add(rendered); serial_put(mask.serial, rendered);
        for (int scenario = 0; scenario < 3; scenario++) {
            dm_guest_write_prepare(backing, span); memset(backing, 0, span);
            mtl_sub_begin(); op_hdr end = {OP_END, 8}; mtl_exec((const uint8_t *)&end);
            MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
            rp.colorAttachments[0].texture = rendered; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(.25, 1, .75, 1);
            rp.renderTargetWidth = rp.renderTargetHeight = side;
            id<MTLRenderCommandEncoder> en = enc_r_begin(rp); enc_r_end(en);
            wb_note(&mask); mtl_sub_commit();
            if (scenario == 1) { dm_guest_write_prepare(backing, span); backing[mask.L.size - 1] = 77; }
            if (scenario == 2) {
                flush((const NVNmemoryPool *)&cached, 1, 0);
                flush((const NVNmemoryPool *)&cached, mask.L.size + 8, 2);
                if (backing[1]) MDIE("mapped flush probe wrote back an empty/unrelated range");
                checked++;
            }
            flush((const NVNmemoryPool *)&cached, 1, 2);
            for (size_t i = 0; i < (size_t)side * side; i++) {
                uint8_t want = scenario == 1 ? 0 : 255;
                if (scenario == 1 && i * bpp + 1 == mask.L.size - 1) want = 77;
                if (backing[i * bpp + 1] != want) MDIE("mapped flush probe layout %d scenario %d pixel %zu: %u != %u", layout, scenario, i, backing[i * bpp + 1], want);
                checked++;
            }
            if (scenario == 1 && backing[mask.L.size - 1] != 77) MDIE("mapped flush probe lost a later CPU write");
        }
        mtl_finish(); serial_del(mask.serial); defer_obj_release(rendered);
        nvn_dirty_remove_range(backing); mtl_pool_del(backing); munmap(backing, span);
    }
#endif
    return checked;
}
