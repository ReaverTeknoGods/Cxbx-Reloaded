// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
// ******************************************************************
// *
// *  This file is part of the Cxbx project.
// *
// *  Cxbx and Cxbe are free software; you can redistribute them
// *  and/or modify them under the terms of the GNU General Public
// *  License as published by the Free Software Foundation; either
// *  version 2 of the license, or (at your option) any later version.
// *
// *  This program is distributed in the hope that it will be useful,
// *  but WITHOUT ANY WARRANTY; without even the implied warranty of
// *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// *  GNU General Public License for more details.
// *
// *  You should have recieved a copy of the GNU General Public License
// *  along with this program; see the file COPYING.
// *  If not, write to the Free Software Foundation, Inc.,
// *  59 Temple Place - Suite 330, Bostom, MA 02111-1307, USA.
// *
// *  This file is heavily based on code from XQEMU
// *  https://github.com/xqemu/xqemu/blob/master/hw/xbox/nv2a/nv2a_pfifo.c
// *  Copyright (c) 2012 espes
// *  Copyright (c) 2015 Jannik Vogel
// *  Copyright (c) 2018 Matt Borgerson
// *
// *  Contributions for Cxbx-Reloaded
// *  Copyright (c) 2017-2018 Luke Usher <luke.usher@outlook.com>
// *  Copyright (c) 2018 Patrick van Logchem <pvanlogchem@gmail.com>
// *
// *  All rights reserved
// *
// ******************************************************************

typedef struct RAMHTEntry {
	uint32_t handle;
	xbox::addr_xt instance;
	enum FIFOEngine engine;
	unsigned int channel_id : 5;
	bool valid;
} RAMHTEntry;

static RAMHTEntry ramht_lookup(NV2AState *d, uint32_t handle); // forward declaration
// Shadow the active transform constant load pointer at the PFIFO DMA parser level
// so queued ranges that start mid constant stream can still be decoded correctly.
static uint32_t s_dmaConstLoadShadow = 0;

/* PFIFO - MMIO and DMA FIFO submission to PGRAPH and VPE */
DEVICE_READ32(PFIFO)
{
    qemu_mutex_lock(&d->pfifo.pfifo_lock);

	DEVICE_READ32_SWITCH() {
	case NV_PFIFO_RAMHT:
		result = 0x03000100; // = NV_PFIFO_RAMHT_SIZE_4K | NV_PFIFO_RAMHT_BASE_ADDRESS(NumberOfPaddingBytes >> 12) | NV_PFIFO_RAMHT_SEARCH_128
		break;
	case NV_PFIFO_RAMFC:
		result = 0x00890110; // = ? | NV_PFIFO_RAMFC_SIZE_2K | ?
		break;
	case NV_PFIFO_INTR_0:
		result = d->pfifo.pending_interrupts;
		break;
	case NV_PFIFO_INTR_EN_0:
		result = d->pfifo.enabled_interrupts;
		break;
	case NV_PFIFO_RUNOUT_STATUS:
		result = NV_PFIFO_RUNOUT_STATUS_LOW_MARK; /* low mark empty */
		break;
	default:
		DEVICE_READ32_REG(pfifo); // Was : DEBUG_READ32_UNHANDLED(PFIFO);
		break;
	}

    qemu_mutex_unlock(&d->pfifo.pfifo_lock);

	DEVICE_READ32_END(PFIFO);
}

DEVICE_WRITE32(PFIFO)
{
    qemu_mutex_lock(&d->pfifo.pfifo_lock);

	switch(addr) {
		case NV_PFIFO_INTR_0:
			d->pfifo.pending_interrupts &= ~value;
			update_irq(d);
			break;
		case NV_PFIFO_INTR_EN_0:
			d->pfifo.enabled_interrupts = value;
			update_irq(d);
			break;
		default:
			DEVICE_WRITE32_REG(pfifo); // Was : DEBUG_WRITE32_UNHANDLED(PFIFO);
			break;
	}

    qemu_cond_broadcast(&d->pfifo.pusher_cond);
    qemu_cond_broadcast(&d->pfifo.puller_cond);

    qemu_mutex_unlock(&d->pfifo.pfifo_lock);

	DEVICE_WRITE32_END(PFIFO);
}

static void pfifo_run_puller(NV2AState *d)
{
    uint32_t *pull0 = &d->pfifo.regs[NV_PFIFO_CACHE1_PULL0];
    uint32_t *pull1 = &d->pfifo.regs[NV_PFIFO_CACHE1_PULL1];
    uint32_t *engine_reg = &d->pfifo.regs[NV_PFIFO_CACHE1_ENGINE];

    uint32_t *status = &d->pfifo.regs[NV_PFIFO_CACHE1_STATUS];
    uint32_t *get_reg = &d->pfifo.regs[NV_PFIFO_CACHE1_GET];
    uint32_t *put_reg = &d->pfifo.regs[NV_PFIFO_CACHE1_PUT];

    // TODO
    // CacheEntry working_cache[NV2A_CACHE1_SIZE];
    // int working_cache_size = 0;
    // pull everything into our own queue

    // TODO think more about locking

    while (true) {
        if (!GET_MASK(*pull0, NV_PFIFO_CACHE1_PULL0_ACCESS)) return;

        /* empty cache1 */
        if (*status & NV_PFIFO_CACHE1_STATUS_LOW_MARK) break;

        uint32_t get = *get_reg;
        uint32_t put = *put_reg;

        assert(get < 128*4 && (get % 4) == 0);
        uint32_t method_entry = d->pfifo.regs[NV_PFIFO_CACHE1_METHOD + get*2];
        uint32_t parameter = d->pfifo.regs[NV_PFIFO_CACHE1_DATA + get*2];

        uint32_t new_get = (get+4) & 0x1fc;
        *get_reg = new_get;

        if (new_get == put) {
            // set low mark
            *status |= NV_PFIFO_CACHE1_STATUS_LOW_MARK;
        }
        if (*status & NV_PFIFO_CACHE1_STATUS_HIGH_MARK) {
            // unset high mark
            *status &= ~NV_PFIFO_CACHE1_STATUS_HIGH_MARK;
            // signal pusher
            qemu_cond_signal(&d->pfifo.pusher_cond);            
        }


        uint32_t method = method_entry & 0x1FFC;
        uint32_t subchannel = GET_MASK(method_entry, NV_PFIFO_CACHE1_METHOD_SUBCHANNEL);

        // NV2A_DPRINTF("pull %d 0x%08X 0x%08X - subch %d\n", get/4, method_entry, parameter, subchannel);

        if (method == 0) {
            RAMHTEntry entry = ramht_lookup(d, parameter);
            assert(entry.valid);

            // assert(entry.channel_id == state->channel_id);

            assert(entry.engine == ENGINE_GRAPHICS);


            /* the engine is bound to the subchannel */
            assert(subchannel < 8);
            SET_MASK(*engine_reg, 3 << (4*subchannel), entry.engine);
            SET_MASK(*pull1, NV_PFIFO_CACHE1_PULL1_ENGINE, entry.engine);
            // NV2A_DPRINTF("engine_reg1 %d 0x%08X\n", subchannel, *engine_reg);


            // TODO: this is fucked
            qemu_mutex_lock(&d->pgraph.pgraph_lock);
            //make pgraph busy
            qemu_mutex_unlock(&d->pfifo.pfifo_lock);

            pgraph_switch_context(d, entry.channel_id);
            pgraph_wait_fifo_access(d);
            pgraph_handle_method(d, subchannel, 0, entry.instance);

            // make pgraph not busy
            qemu_mutex_unlock(&d->pgraph.pgraph_lock);
            qemu_mutex_lock(&d->pfifo.pfifo_lock);

        } else if (method >= 0x100) {
            // method passed to engine

            /* methods that take objects.
             * TODO: Check this range is correct for the nv2a */
            if (method >= 0x180 && method < 0x200) {
                //qemu_mutex_lock_iothread();
                RAMHTEntry entry = ramht_lookup(d, parameter);
                assert(entry.valid);
                // assert(entry.channel_id == state->channel_id);
                parameter = entry.instance;
                //qemu_mutex_unlock_iothread();
            }

            enum FIFOEngine engine = (enum FIFOEngine)GET_MASK(*engine_reg, 3 << (4*subchannel));
            // NV2A_DPRINTF("engine_reg2 %d 0x%08X\n", subchannel, *engine_reg);
            assert(engine == ENGINE_GRAPHICS);
            SET_MASK(*pull1, NV_PFIFO_CACHE1_PULL1_ENGINE, engine);

            // TODO: this is fucked
            qemu_mutex_lock(&d->pgraph.pgraph_lock);
            //make pgraph busy
            qemu_mutex_unlock(&d->pfifo.pfifo_lock);

            pgraph_wait_fifo_access(d);
            pgraph_handle_method(d, subchannel, method, parameter);

            // make pgraph not busy
            qemu_mutex_unlock(&d->pgraph.pgraph_lock);
            qemu_mutex_lock(&d->pfifo.pfifo_lock);
        } else {
            assert(false);
        }

    }
}

int pfifo_puller_thread(NV2AState *d)
{
    g_AffinityPolicy->SetAffinityOther();
    CxbxSetThreadName("Cxbx NV2A FIFO puller");

    glo_set_current(d->pgraph.gl_context);

    qemu_mutex_lock(&d->pfifo.pfifo_lock);
    while (true) {
        pfifo_run_puller(d);
        qemu_cond_wait(&d->pfifo.puller_cond, &d->pfifo.pfifo_lock);

        if (d->exiting) {
            break;
        }
    }
    qemu_mutex_unlock(&d->pfifo.pfifo_lock);

	glo_set_current(NULL); // Cxbx addition

	return NULL;
}

static void pfifo_run_pusher(NV2AState *d)
{
    uint32_t *push0 = &d->pfifo.regs[NV_PFIFO_CACHE1_PUSH0];
    uint32_t *push1 = &d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1];
    uint32_t *dma_subroutine = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_SUBROUTINE];
    uint32_t *dma_state = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_STATE];
    uint32_t *dma_push = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUSH];
    uint32_t *dma_get = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    uint32_t *dma_put = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];
    uint32_t *dma_dcount = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_DCOUNT];

    uint32_t *status = &d->pfifo.regs[NV_PFIFO_CACHE1_STATUS];
    uint32_t *get_reg = &d->pfifo.regs[NV_PFIFO_CACHE1_GET];
    uint32_t *put_reg = &d->pfifo.regs[NV_PFIFO_CACHE1_PUT];

    if (!GET_MASK(*push0, NV_PFIFO_CACHE1_PUSH0_ACCESS)) return;
    if (!GET_MASK(*dma_push, NV_PFIFO_CACHE1_DMA_PUSH_ACCESS)) return;

    /* suspended */
    if (GET_MASK(*dma_push, NV_PFIFO_CACHE1_DMA_PUSH_STATUS)) return;

    // TODO: should we become busy here??
    // NV_PFIFO_CACHE1_DMA_PUSH_STATE _BUSY

    unsigned int channel_id = GET_MASK(*push1,
                                       NV_PFIFO_CACHE1_PUSH1_CHID);


	/* Channel running DMA */
	uint32_t channel_modes = d->pfifo.regs[NV_PFIFO_MODE];
	assert(channel_modes & (1 << channel_id));

    assert(GET_MASK(*push1, NV_PFIFO_CACHE1_PUSH1_MODE)
            == NV_PFIFO_CACHE1_PUSH1_MODE_DMA);

	/* We're running so there should be no pending errors... */
    assert(GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR)
            == NV_PFIFO_CACHE1_DMA_STATE_ERROR_NONE);

    hwaddr dma_instance =
        GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_DMA_INSTANCE],
                 NV_PFIFO_CACHE1_DMA_INSTANCE_ADDRESS_MASK) << 4; // TODO : Use NV_PFIFO_CACHE1_DMA_INSTANCE_ADDRESS_MOVE?

    hwaddr dma_len;
    uint8_t *dma = (uint8_t*)nv_dma_map(d, dma_instance, &dma_len);

	/* based on the convenient pseudocode in envytools */
    while (true) {
        uint32_t dma_get_v = *dma_get;
        uint32_t dma_put_v = *dma_put;
        if (dma_get_v == dma_put_v) break;
        if (dma_get_v >= dma_len) {
            assert(false);
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR,
                     NV_PFIFO_CACHE1_DMA_STATE_ERROR_PROTECTION);
            break;
        }

        uint32_t word = ldl_le_p((uint32_t*)(dma + dma_get_v));
        dma_get_v += 4;

        uint32_t method_type =
            GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE);
        uint32_t method_subchannel =
            GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL);
        uint32_t method =
            GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD) << 2;
        uint32_t method_count =
            GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT);

        uint32_t subroutine_state =
            GET_MASK(*dma_subroutine, NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE);

        if (method_count) {
            /* full */
            if (*status & NV_PFIFO_CACHE1_STATUS_HIGH_MARK) return;


            /* data word of methods command */
            d->pfifo.regs[NV_PFIFO_CACHE1_DMA_DATA_SHADOW] = word;

            uint32_t put = *put_reg;
            uint32_t get = *get_reg;

            assert((method & 3) == 0);
            uint32_t method_entry = 0;
            SET_MASK(method_entry, NV_PFIFO_CACHE1_METHOD_ADDRESS, method >> 2);
            SET_MASK(method_entry, NV_PFIFO_CACHE1_METHOD_TYPE, method_type);
            SET_MASK(method_entry, NV_PFIFO_CACHE1_METHOD_SUBCHANNEL, method_subchannel);

            // NV2A_DPRINTF("push %d 0x%08X 0x%08X - subch %d\n", put/4, method_entry, word, method_subchannel);

            assert(put < 128*4 && (put%4) == 0);
            d->pfifo.regs[NV_PFIFO_CACHE1_METHOD + put*2] = method_entry;
            d->pfifo.regs[NV_PFIFO_CACHE1_DATA + put*2] = word;

            if (method == NV097_SET_TRANSFORM_CONSTANT_LOAD) {
                s_dmaConstLoadShadow = word;
            }
            else if (method >= NV097_SET_TRANSFORM_CONSTANT
                && method < NV097_SET_TRANSFORM_CONSTANT + 32 * 4) {
                unsigned slot = (method - NV097_SET_TRANSFORM_CONSTANT) / 4;
                if ((slot % 4) == 3 && s_dmaConstLoadShadow < NV2A_VERTEXSHADER_CONSTANTS) {
                    s_dmaConstLoadShadow++;
                }
            }

            uint32_t new_put = (put+4) & 0x1fc;
            *put_reg = new_put;
            if (new_put == get) {
                // set high mark
                *status |= NV_PFIFO_CACHE1_STATUS_HIGH_MARK;
            }
            if (*status & NV_PFIFO_CACHE1_STATUS_LOW_MARK) {
                // unset low mark
                *status &= ~NV_PFIFO_CACHE1_STATUS_LOW_MARK;
                // signal puller
                qemu_cond_signal(&d->pfifo.puller_cond);
            }

            if (method_type == NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_INC) {
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD,
                         (method + 4) >> 2);
            }
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,
                     method_count - 1);
            (*dma_dcount)++;
		} else {
			/* no command active - this is the first word of a new one */
            d->pfifo.regs[NV_PFIFO_CACHE1_DMA_RSVD_SHADOW] = word;

			/* match all forms */
			if ((word & 0xe0000003) == 0x20000000) {
				/* old jump */
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET_JMP_SHADOW] =
                    dma_get_v;
                dma_get_v = word & 0x1fffffff;
				NV2A_DPRINTF("pb OLD_JMP 0x%08X\n", dma_get_v);
			} else if ((word & 3) == 1) {
				/* jump */
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET_JMP_SHADOW] =
                    dma_get_v;
                dma_get_v = word & 0xfffffffc;
				NV2A_DPRINTF("pb JMP 0x%08X\n", dma_get_v);
			} else if ((word & 3) == 2) {
				/* call */
                if (subroutine_state) {
                    SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR,
                             NV_PFIFO_CACHE1_DMA_STATE_ERROR_CALL);
                    break;
                } else {
                    *dma_subroutine = dma_get_v;
                    SET_MASK(*dma_subroutine,
                             NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE, 1);
                    dma_get_v = word & 0xfffffffc;
                    NV2A_DPRINTF("pb CALL 0x%08X\n", dma_get_v);
                }
            } else if (word == 0x00020000) {
                /* return */
                if (!subroutine_state) {
                    SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR,
                             NV_PFIFO_CACHE1_DMA_STATE_ERROR_RETURN);
                    // break;
                } else {
                    dma_get_v = *dma_subroutine & 0xfffffffc;
                    SET_MASK(*dma_subroutine,
                             NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE, 0);
                    NV2A_DPRINTF("pb RET 0x%08X\n", dma_get_v);
                }
            } else if ((word & 0xe0030003) == 0) {
                /* increasing methods */
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD,
                         (word & 0x1fff) >> 2 );
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL,
                         (word >> 13) & 7);
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,
                         (word >> 18) & 0x7ff);
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE,
                         NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_INC);
                *dma_dcount = 0;
            } else if ((word & 0xe0030003) == 0x40000000) {
                /* non-increasing methods */
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD,
                         (word & 0x1fff) >> 2 );
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL,
                         (word >> 13) & 7);
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,
                         (word >> 18) & 0x7ff);
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE,
                         NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_NON_INC);
                *dma_dcount = 0;
            } else {
                NV2A_DPRINTF("pb reserved cmd 0x%08X - 0x%08X\n",
                             dma_get_v, word);
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR,
                         NV_PFIFO_CACHE1_DMA_STATE_ERROR_RESERVED_CMD);
                // break;
                assert(false);
            }
        }

        *dma_get = dma_get_v;

        if (GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR)) {
            break;
        }
    }

    // NV2A_DPRINTF("DMA pusher done: max 0x%08X, 0x%08X - 0x%08X\n",
    //      dma_len, control->dma_get, control->dma_put);

    uint32_t error = GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_ERROR);
    if (error) {
        NV2A_DPRINTF("pb error: %d\n", error);
        assert(false);

        SET_MASK(*dma_push, NV_PFIFO_CACHE1_DMA_PUSH_STATUS, 1); /* suspended */

        // d->pfifo.pending_interrupts |= NV_PFIFO_INTR_0_DMA_PUSHER;
        // update_irq(d);
    }
}

int pfifo_pusher_thread(NV2AState *d)
{
    g_AffinityPolicy->SetAffinityOther();
    CxbxSetThreadName("Cxbx NV2A FIFO pusher");

    qemu_mutex_lock(&d->pfifo.pfifo_lock);
    while (true) {
        pfifo_run_pusher(d);
        qemu_cond_wait(&d->pfifo.pusher_cond, &d->pfifo.pfifo_lock);

        if (d->exiting) {
            break;
        }
    }
    qemu_mutex_unlock(&d->pfifo.pfifo_lock);

	return 0;
}

unsigned int ramht_size(NV2AState *d)
{
	return 
		1 << (GET_MASK(d->pfifo.regs[NV_PFIFO_RAMHT], NV_PFIFO_RAMHT_SIZE_MASK) + 12);
}

static uint32_t ramht_hash(NV2AState *d, uint32_t handle)
{
	/* XXX: Think this is different to what nouveau calculates... */
	unsigned int bits = ffs(ramht_size(d)) - 2;

	uint32_t hash = 0;
	while (handle) {
		hash ^= (handle & ((1 << bits) - 1));
		handle >>= bits;
	}

    unsigned int channel_id = GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                                       NV_PFIFO_CACHE1_PUSH1_CHID);
    hash ^= channel_id << (bits - 4);

	return hash;
}

static RAMHTEntry ramht_lookup(NV2AState *d, uint32_t handle)
{
	uint32_t hash = ramht_hash(d, handle);
	assert(hash * 8 < ramht_size(d));

	xbox::addr_xt ramht_address =
		GET_MASK(d->pfifo.regs[NV_PFIFO_RAMHT],
			NV_PFIFO_RAMHT_BASE_ADDRESS_MASK) << 12;

	uint8_t *entry_ptr = d->pramin.ramin_ptr + ramht_address + hash * 8;

	uint32_t entry_handle = ldl_le_p((uint32_t*)entry_ptr);
	uint32_t entry_context = ldl_le_p((uint32_t*)(entry_ptr + 4));

	RAMHTEntry entry;
	entry.handle = entry_handle;
	entry.instance = (entry_context & NV_RAMHT_INSTANCE) << 4;
	entry.engine = (FIFOEngine)((entry_context & NV_RAMHT_ENGINE) >> 16);
	entry.channel_id = (entry_context & NV_RAMHT_CHID) >> 24;
	entry.valid = entry_context & NV_RAMHT_STATUS;

	return entry;
}

// ---------------------------------------------------------------------------
// Inline push buffer constant extraction
// ---------------------------------------------------------------------------
// Parse queued push-buffer DMA ranges and extract
// NV097_SET_TRANSFORM_CONSTANT / NV097_SET_TRANSFORM_CONSTANT_LOAD writes.
// The extracted values are written into a side buffer so the HLE constant
// upload path can overlay the latest constants for the current draw without
// waiting for the PFIFO pusher+puller threads to finish.
//
// PFIFO threads still run in parallel and will process the same data
// (idempotent for constants). No locks are held because:
//   - Push buffer memory is stable (game can't overwrite until DMA_GET
//     advances, and we're on the game's thread so it can't run).
//   - We only write the parser side buffer here; pg->vsh_constants remains
//     owned by the PFIFO puller.
//   - Duplicate writes from PFIFO are harmless because the overlay is only
//     used for the ranges captured since the last draw.
//
// NV_USER_DMA_PUT writes enqueue the unread [start, newPut) suffix of each PUT
// advance. At draw time,
// CxbxUpdateHostVertexShaderConstants consumes every pending range, clears the
// side buffer once, parses each queued range into that buffer, snapshots
// pg->vsh_constants, and overlays the parsed values into the snapshot copy.
// This ensures each draw uses the latest queued constants, immune to stale
// writes from the PFIFO puller processing older batches.
// pg->vsh_constants is NOT modified — the PFIFO puller is the sole writer there.
// ---------------------------------------------------------------------------

// --- DMA_PUT kick tracking ---
// Queue every runtime DMA_PUT advance so the HLE draw path can parse all pending
// push-buffer ranges without issuing its own KickOff calls.
#define NV2A_MAX_PENDING_DMA_PUT_RANGES 4096

typedef struct PendingDmaPutRange {
    uint32_t start;
    uint32_t end;
    uint32_t dma_get;
    uint32_t dma_state;
    uint32_t dma_subroutine;
    uint32_t dma_const_load;
} PendingDmaPutRange;

static PendingDmaPutRange s_pendingDmaPutRanges[NV2A_MAX_PENDING_DMA_PUT_RANGES];
static volatile LONG s_pendingDmaPutRead = 0;
static volatile LONG s_pendingDmaPutWrite = 0;
static volatile LONG s_pendingDmaPutHighWater = 0;
static volatile LONG s_pendingDmaPutOverflowCount = 0;

static LONG nv2a_pfifo_pending_dma_depth(LONG read, LONG write)
{
    return (write >= read)
        ? (write - read)
        : (write + NV2A_MAX_PENDING_DMA_PUT_RANGES - read);
}

static void nv2a_pfifo_note_pending_dma_depth(LONG read, LONG write)
{
    LONG depth = nv2a_pfifo_pending_dma_depth(read, write);
    LONG highWater = s_pendingDmaPutHighWater;

    while (depth > highWater) {
        LONG previous = InterlockedCompareExchange(&s_pendingDmaPutHighWater, depth, highWater);
        if (previous == highWater) {
            break;
        }
        highWater = previous;
    }
}

void nv2a_pfifo_notify_dma_put_write(uint32_t oldValue, uint32_t newValue,
	                                 uint32_t dmaGet, uint32_t dmaState, uint32_t dmaSubroutine)
{
    if (oldValue == newValue) {
        return;
    }

    LONG write = s_pendingDmaPutWrite;
    LONG read = s_pendingDmaPutRead;
    LONG next = (write + 1) % NV2A_MAX_PENDING_DMA_PUT_RANGES;

    if (next == read) {
        InterlockedIncrement(&s_pendingDmaPutOverflowCount);
        InterlockedExchange(&s_pendingDmaPutHighWater, NV2A_MAX_PENDING_DMA_PUT_RANGES - 1);
        LONG last = (write + NV2A_MAX_PENDING_DMA_PUT_RANGES - 1) % NV2A_MAX_PENDING_DMA_PUT_RANGES;
        s_pendingDmaPutRanges[last].end = newValue;
        return;
    }

    uint32_t parseStart = oldValue;
    if (oldValue < dmaGet && dmaGet <= newValue) {
        parseStart = dmaGet;
    }

    s_pendingDmaPutRanges[write].start = parseStart;
    s_pendingDmaPutRanges[write].end = newValue;
    s_pendingDmaPutRanges[write].dma_get = dmaGet;
    s_pendingDmaPutRanges[write].dma_state = dmaState;
    s_pendingDmaPutRanges[write].dma_subroutine = dmaSubroutine;
    s_pendingDmaPutRanges[write].dma_const_load = s_dmaConstLoadShadow;
    InterlockedExchange(&s_pendingDmaPutWrite, next);
    nv2a_pfifo_note_pending_dma_depth(read, next);
}

// Side buffer for inline-parsed constants — valid for the pending DMA_PUT ranges
// consumed by the current draw only.
static uint32_t s_parsedConstants[NV2A_VERTEXSHADER_CONSTANTS][4];
static bool     s_parsedSlotDirty[NV2A_VERTEXSHADER_CONSTANTS]; // which slots we've parsed

// Diagnostic log file for inline constant parser debugging
static FILE *s_constDiagLog = nullptr;
static int s_constDiagDrawNum = 0;
static int s_constDiagFrameNum = 0;

FILE *nv2a_get_const_diag_log()
{
    return nullptr;
}

int nv2a_const_diag_get_frame() { return s_constDiagFrameNum; }
int nv2a_const_diag_get_draw() { return s_constDiagDrawNum; }

void nv2a_const_diag_new_frame()
{
	s_constDiagFrameNum++;
	s_constDiagDrawNum = 0;
}

void nv2a_const_diag_next_draw()
{
	s_constDiagDrawNum++;
}

void nv2a_clear_parsed_constants()
{
	memset(s_parsedSlotDirty, 0, sizeof(s_parsedSlotDirty));
}

void nv2a_copy_parsed_constants(uint32_t *outConstants, uint8_t *outDirtyMask)
{
    if (outConstants != nullptr) {
        memcpy(outConstants, s_parsedConstants, sizeof(s_parsedConstants));
    }

    if (outDirtyMask != nullptr) {
        for (int i = 0; i < NV2A_VERTEXSHADER_CONSTANTS; i++) {
            outDirtyMask[i] = s_parsedSlotDirty[i] ? 1 : 0;
        }
    }
}

// Overlay parsed constants onto a snapshot copy (NOT pg->vsh_constants).
// Called after memcpy'ing pg->vsh_constants to local_constants.
// Also logs diagnostic info about what was overlayed.
static int s_lastOverlayDirtyCount = 0;

int nv2a_overlay_last_dirty_count() { return s_lastOverlayDirtyCount; }

void nv2a_overlay_parsed_constants(float *local_constants)
{
	int overlayCount = 0;
	int differCount = 0;
	int firstDirty = -1, lastDirty = -1;
	for (int i = 0; i < NV2A_VERTEXSHADER_CONSTANTS; i++) {
		if (s_parsedSlotDirty[i]) {
			if (firstDirty < 0) firstDirty = i;
			lastDirty = i;
			overlayCount++;
			if (memcmp(&local_constants[i * 4], s_parsedConstants[i], 4 * sizeof(float)) != 0)
				differCount++;
			memcpy(&local_constants[i * 4], s_parsedConstants[i], 4 * sizeof(float));
		}
	}
	s_lastOverlayDirtyCount = overlayCount;

	FILE *f = nv2a_get_const_diag_log();
	if (f && (s_constDiagFrameNum <= 10 || (s_constDiagFrameNum % 60 == 0))) {
		fprintf(f, "  OVERLAY F%d D%d: %d slots dirty [%d..%d], %d differed from snapshot\n",
			s_constDiagFrameNum, s_constDiagDrawNum, overlayCount, firstDirty, lastDirty, differCount);
		// Log first few bone constants (slots 4-12) — snapshot vs overlay
		if (overlayCount > 0 && firstDirty >= 0 && firstDirty < 20) {
			for (int i = firstDirty; i <= lastDirty && i < firstDirty + 6; i++) {
				if (s_parsedSlotDirty[i]) {
					float *snap = &local_constants[i * 4]; // already overlaid
					fprintf(f, "    c[%d] = %.4f %.4f %.4f %.4f\n",
						i, snap[0], snap[1], snap[2], snap[3]);
				}
			}
		}
		fflush(f);
	}
}

static bool nv2a_pfifo_is_hle_mirrored_texture_switch_method(uint32_t method)
{
    for (uint32_t stage = 0; stage < 4; ++stage) {
        uint32_t stage_base = NV097_SET_TEXTURE_OFFSET + stage * 0x40;
        if (method == stage_base || method == stage_base + 4) {
            return true;
        }
    }

    return false;
}

static bool nv2a_pfifo_is_hle_mirrored_texture_state_method(uint32_t method)
{
    constexpr uint32_t single_dword_stage_bases[] = {
        NV097_SET_TEXTURE_CONTROL0,
        NV097_SET_TEXTURE_CONTROL1,
        NV097_SET_TEXTURE_FILTER,
        NV097_SET_TEXTURE_IMAGE_RECT,
        NV097_SET_TEXTURE_SET_BUMP_ENV_SCALE,
        NV097_SET_TEXTURE_SET_BUMP_ENV_OFFSET,
    };

    for (uint32_t base : single_dword_stage_bases) {
        uint32_t delta = method - base;
        if (method >= base && delta < 4 * 0x40 && (delta % 0x40) == 0) {
            return true;
        }
    }

    uint32_t bump_env_delta = method - NV097_SET_TEXTURE_SET_BUMP_ENV_MAT;
    return method >= NV097_SET_TEXTURE_SET_BUMP_ENV_MAT
        && bump_env_delta < 4 * 0x40
        && (bump_env_delta % 0x40) < 0x10
        && (bump_env_delta % 4) == 0;
}

static bool nv2a_pfifo_is_hle_mirrored_viewport_method(uint32_t method)
{
    if (method >= NV097_SET_VIEWPORT_OFFSET
        && method < NV097_SET_VIEWPORT_OFFSET + 0x10) {
        return true;
    }

    if (method >= NV097_SET_VIEWPORT_SCALE
        && method < NV097_SET_VIEWPORT_SCALE + 0x10) {
        return true;
    }

    return method == NV097_SET_CLIP_MIN
        || method == NV097_SET_CLIP_MAX;
}

static bool nv2a_pfifo_is_hle_window_clip_method(uint32_t method)
{
    if (method == NV097_SET_WINDOW_CLIP_TYPE) {
        return true;
    }

    constexpr uint32_t window_clip_bases[] = {
        NV097_SET_WINDOW_CLIP_HORIZONTAL,
        NV097_SET_WINDOW_CLIP_VERTICAL,
    };

    for (uint32_t base : window_clip_bases) {
        uint32_t delta = method - base;
        if (method >= base && delta < 8 * 4 && (delta % 4) == 0) {
            return true;
        }
    }

    return false;
}

static bool nv2a_pfifo_is_hle_ignored_eye_vector_method(uint32_t method)
{
    uint32_t delta = method - NV097_SET_EYE_VECTOR;
    return method >= NV097_SET_EYE_VECTOR
        && delta < 3 * 4
        && (delta % 4) == 0;
}

static bool nv2a_pfifo_is_hle_mirrored_texgen_method(uint32_t method)
{
    constexpr uint32_t texgen_bases[] = {
        NV097_SET_TEXGEN_S,
        NV097_SET_TEXGEN_T,
        NV097_SET_TEXGEN_R,
        NV097_SET_TEXGEN_Q,
    };

    for (uint32_t base : texgen_bases) {
        uint32_t delta = method - base;
        if (method >= base && delta < 0x40 && (delta % 0x10) == 0) {
            return true;
        }
    }

    return false;
}

static bool nv2a_pfifo_is_hle_mirrored_non_blocking_method(uint32_t method)
{
    if (nv2a_pfifo_is_hle_mirrored_viewport_method(method)) {
        return true;
    }

    if (nv2a_pfifo_is_hle_mirrored_texgen_method(method)) {
        return true;
    }

    switch (method) {
    case NV097_SET_SHADER_CLIP_PLANE_MODE:
    case NV097_SET_SHADER_OTHER_STAGE_INPUT:
    case 0x00001E74:
        return true;
    default:
        break;
    }

    // These writes are already mirrored immediately by the HLE D3D8 patches,
    // are derived from pixel-container metadata in the D3D9 HLE path, or are
    // not consumed by the current D3D9 HLE draw path, so they do not require
    // a PFIFO drain just to keep host state in sync here.
    switch (method) {
    case NV097_SET_ALPHA_TEST_ENABLE:
    case NV097_SET_ALPHA_FUNC:
    case NV097_SET_BLEND_ENABLE:
    case NV097_WAIT_FOR_IDLE:
    case NV097_SET_CULL_FACE_ENABLE:
    case NV097_SET_CULL_FACE:
    case NV097_SET_ALPHA_REF:
    case NV097_SET_BLEND_FUNC_SFACTOR:
    case NV097_SET_BLEND_FUNC_DFACTOR:
    case NV097_SET_DEPTH_FUNC:
    case NV097_SET_DEPTH_MASK:
    case NV097_SET_STENCIL_TEST_ENABLE:
    case NV097_SET_STENCIL_FUNC:
    case NV097_SET_STENCIL_FUNC_REF:
    case NV097_SET_STENCIL_FUNC_MASK:
    case NV097_SET_STENCIL_OP_FAIL:
    case NV097_SET_STENCIL_OP_ZFAIL:
    case NV097_SET_STENCIL_OP_ZPASS:
    case NV097_SET_FOG_COLOR:
    case NV097_SET_TRANSFORM_EXECUTION_MODE:
    case NV097_SET_TRANSFORM_PROGRAM_START:
    case NV097_SET_SURFACE_PITCH:
    case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE:
    case NV097_SET_ZMIN_MAX_CONTROL:
    case 0x00001D7C:
    case NV097_SET_COMPRESS_ZBUFFER_EN:
    case NV097_SET_COMPRESS_ZBUFFER_EN + 4:
    case NV097_SET_COLOR_CLEAR_VALUE:
    case NV097_SET_SURFACE_COLOR_OFFSET:
    case NV097_SET_SURFACE_ZETA_OFFSET:
    case NV097_SET_CONTROL0:
    case NV097_SET_DEPTH_TEST_ENABLE:
    case NV097_SET_SURFACE_FORMAT:
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        return true;
    default:
        return nv2a_pfifo_is_hle_window_clip_method(method)
            || nv2a_pfifo_is_hle_ignored_eye_vector_method(method)
            || nv2a_pfifo_is_hle_mirrored_texture_switch_method(method)
            || nv2a_pfifo_is_hle_mirrored_texture_state_method(method);
    }
}

static bool nv2a_pfifo_is_inline_vertex_data_method(uint32_t method)
{
    return method >= NV097_SET_VERTEX_DATA2F_M
        && method < NV097_SET_TEXTURE_OFFSET;
}

static bool nv2a_pfifo_is_non_blocking_register_combiner_method(uint32_t method)
{
    constexpr uint32_t combiner_ranges[][2] = {
        { NV097_SET_SPECULAR_FOG_FACTOR, 2 },
        { NV097_SET_COMBINER_ALPHA_ICW, 8 },
        { NV097_SET_COMBINER_COLOR_ICW, 8 },
        { NV097_SET_COMBINER_ALPHA_OCW, 8 },
        { NV097_SET_COMBINER_COLOR_OCW, 8 },
        { NV097_SET_COMBINER_FACTOR0, 8 },
        { NV097_SET_COMBINER_FACTOR1, 8 },
    };

    for (const auto &range : combiner_ranges) {
        uint32_t base = range[0];
        uint32_t count = range[1];
        if (method >= base && method < base + count * 4) {
            return true;
        }
    }

    switch (method) {
    case NV097_SET_COMBINER_SPECULAR_FOG_CW0:
    case NV097_SET_COMBINER_SPECULAR_FOG_CW1:
    case NV097_SET_COMBINER_CONTROL:
        return true;
    default:
        return false;
    }
}

static bool nv2a_pfifo_is_non_blocking_transform_program_method(uint32_t method)
{
    if (method == NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN
        || method == NV097_SET_TRANSFORM_PROGRAM_LOAD) {
        return true;
    }

    return method >= NV097_SET_TRANSFORM_PROGRAM
        && method < NV097_SET_TRANSFORM_PROGRAM + 32 * 4;
}

static bool nv2a_pfifo_is_non_blocking_method_for_vs_const_sync(uint32_t method)
{
    if (method == NV097_NO_OPERATION) {
        return true;
    }

    if (nv2a_pfifo_is_inline_vertex_data_method(method)) {
        return true;
    }

    if (nv2a_pfifo_is_non_blocking_transform_program_method(method)) {
        return true;
    }

    if (nv2a_pfifo_is_non_blocking_register_combiner_method(method)) {
        return true;
    }

    return nv2a_pfifo_is_hle_mirrored_non_blocking_method(method);
}

static bool nv2a_pfifo_extract_constants_inline_range(NV2AState *d, uint32_t pbStart, uint32_t pbEnd,
                                                      uint32_t *ioDmaState,
                                                      uint32_t *ioDmaSubroutine,
                                                      uint32_t *ioDmaConstLoad,
                                                      bool *outReachedDrawBoundary,
                                                      bool *outSawNonConstantMethods,
                                                      uint32_t *outFirstNonConstantMethod,
                                                      uint32_t *outConsumedEnd)
{
	uint32_t *pf_regs = d->pfifo.regs;
	uint32_t dmaState = *ioDmaState;
	uint32_t dmaSubroutine = *ioDmaSubroutine;
	uint32_t dmaConstLoad = *ioDmaConstLoad;
    bool reached_draw_boundary = false;
    bool saw_non_constant_methods = false;
    uint32_t first_non_constant_method = 0xFFFFFFFFu;
    if (outReachedDrawBoundary) {
        *outReachedDrawBoundary = false;
    }
    if (outSawNonConstantMethods) {
        *outSawNonConstantMethods = false;
    }
    if (outFirstNonConstantMethod) {
        *outFirstNonConstantMethod = 0xFFFFFFFFu;
    }
    if (outConsumedEnd) {
        *outConsumedEnd = pbStart;
    }

	// Map the DMA push buffer
	hwaddr dma_instance =
		GET_MASK(pf_regs[NV_PFIFO_CACHE1_DMA_INSTANCE],
		         NV_PFIFO_CACHE1_DMA_INSTANCE_ADDRESS_MASK) << 4;
	hwaddr dma_len;
	uint8_t *dma = (uint8_t *)nv_dma_map(d, dma_instance, &dma_len);
	if (!dma) return false;

    // Parse the exact push-buffer range [pbStart, pbEnd) captured from NV_USER_DMA_PUT.
	uint32_t pos = pbStart;
    uint32_t end = pbEnd;
	if (pos == end) return false;

    // Local push buffer parse state
    // A DMA_PUT increment may append data in the middle of an active method stream,
    // so seed the parser from the DMA state captured when the kick happened.
    uint32_t method_type =
        GET_MASK(dmaState, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE);
    uint32_t method =
        GET_MASK(dmaState, NV_PFIFO_CACHE1_DMA_STATE_METHOD) << 2;
    uint32_t method_count =
        GET_MASK(dmaState, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT);
    bool method_inc =
        (method_type == NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_INC);

    // Seed the local constant load pointer from the PFIFO-side shadow so ranges that
    // begin in the middle of SET_TRANSFORM_CONSTANT data words still decode correctly.
    int const_load = (dmaConstLoad < NV2A_VERTEXSHADER_CONSTANTS)
        ? static_cast<int>(dmaConstLoad)
        : -1;
	bool found = false;

	// Subroutine return address (single-level, like hardware)
    uint32_t sub_ret = dmaSubroutine & 0xfffffffc;
    bool sub_active =
        GET_MASK(dmaSubroutine, NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE) != 0;

	// Safety limit to avoid infinite loops on corrupted data
	for (int budget = 65536; budget > 0 && pos != end; --budget) {
		if (pos >= dma_len) break;

		uint32_t word = *(uint32_t *)(dma + pos);
		pos += 4;
        bool stop_after_this_word = false;

		if (method_count) {
			// --- data word for current method ---
			if (method == NV097_SET_TRANSFORM_CONSTANT_LOAD) {
				const_load = word;
			}
			else if (method >= NV097_SET_TRANSFORM_CONSTANT &&
			         method <  NV097_SET_TRANSFORM_CONSTANT + 32 * 4 &&
			         (unsigned)const_load < NV2A_VERTEXSHADER_CONSTANTS) {
				unsigned slot = (method - NV097_SET_TRANSFORM_CONSTANT) / 4;
				// Write ONLY to side buffer — pg->vsh_constants is owned by the puller
				s_parsedConstants[const_load][slot % 4] = word;
				s_parsedSlotDirty[const_load] = true;
				found = true;
				if ((slot % 4) == 3) {
					const_load++;
				}
			}
			else if (!nv2a_pfifo_is_non_blocking_method_for_vs_const_sync(method)) {
                saw_non_constant_methods = true;
                if (first_non_constant_method == 0xFFFFFFFFu) {
                    first_non_constant_method = method;
                }
            }

            if (method == NV097_SET_BEGIN_END
                || method == NV097_DRAW_ARRAYS
                || method == NV097_ARRAY_ELEMENT16
                || method == NV097_ARRAY_ELEMENT32
                || method == NV097_INLINE_ARRAY) {
                stop_after_this_word = true;
            }

			if (method_inc) method += 4;
			method_count--;

            if (stop_after_this_word) {
				reached_draw_boundary = true;
                break;
            }
		}
		else {
			// --- new command header / control flow ---
			if ((word & 0xe0000003) == 0x20000000) {
				// Old jump
				pos = word & 0x1fffffff;
			}
			else if ((word & 3) == 1) {
				// Jump
				pos = word & 0xfffffffc;
			}
			else if ((word & 3) == 2) {
				// Call (single-level)
				if (!sub_active) {
					sub_ret = pos;
					sub_active = true;
					pos = word & 0xfffffffc;
				}
			}
			else if (word == 0x00020000) {
				// Return
				if (sub_active) {
					pos = sub_ret;
					sub_active = false;
				}
			}
			else if ((word & 0xe0030003) == 0) {
				// Increasing methods
				method       = word & 0x1FFC;
				method_count = (word >> 18) & 0x7FF;
                method_type  = NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_INC;
				method_inc   = true;
			}
			else if ((word & 0xe0030003) == 0x40000000) {
				// Non-increasing methods
				method       = word & 0x1FFC;
				method_count = (word >> 18) & 0x7FF;
                method_type  = NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_NON_INC;
				method_inc   = false;
			}
			// else: NOP / padding / reserved — skip
		}
	}

    uint32_t nextDmaState = dmaState;
    SET_MASK(nextDmaState, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE, method_type);
    SET_MASK(nextDmaState, NV_PFIFO_CACHE1_DMA_STATE_METHOD, method >> 2);
    SET_MASK(nextDmaState, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT, method_count);
    *ioDmaState = nextDmaState;

    uint32_t nextDmaSubroutine = sub_ret & 0xfffffffc;
    SET_MASK(nextDmaSubroutine, NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE, sub_active ? 1 : 0);
    *ioDmaSubroutine = nextDmaSubroutine;
    *ioDmaConstLoad = (const_load >= 0)
        ? static_cast<uint32_t>(const_load)
        : NV2A_VERTEXSHADER_CONSTANTS;

    if (outConsumedEnd) {
        *outConsumedEnd = pos;
    }

	if (found) {
		extern bool g_VshConstantsDirtyAny;
		g_VshConstantsDirtyAny = true;
	}

    if (outReachedDrawBoundary) {
        *outReachedDrawBoundary = reached_draw_boundary;
    }
    if (outSawNonConstantMethods) {
        *outSawNonConstantMethods = saw_non_constant_methods;
    }
    if (outFirstNonConstantMethod) {
        *outFirstNonConstantMethod = first_non_constant_method;
    }

	// Log parser results
	FILE *f = nv2a_get_const_diag_log();
	if (f && (s_constDiagFrameNum <= 10 || (s_constDiagFrameNum % 60 == 0))) {
		int dirtyCount = 0;
		int firstDirty = -1, lastDirty = -1;
		for (int i = 0; i < NV2A_VERTEXSHADER_CONSTANTS; i++) {
			if (s_parsedSlotDirty[i]) {
				dirtyCount++;
				if (firstDirty < 0) firstDirty = i;
				lastDirty = i;
			}
		}
		fprintf(f, "PARSE F%d D%d: pbStart=0x%08X end=0x%08X found=%d slotsNowDirty=%d [%d..%d]\n",
			s_constDiagFrameNum, s_constDiagDrawNum, pbStart, end, (int)found, dirtyCount, firstDirty, lastDirty);
		fflush(f);
	}

	return found;
}

static bool nv2a_pfifo_can_reseed_parse_state(uint32_t rangeStart,
                                              uint32_t dmaGet,
                                              uint32_t dmaState,
                                              uint32_t dmaSubroutine)
{
    if (rangeStart == dmaGet) {
        return true;
    }

    // A queued DMA_PUT segment can still be safe to parse from its own start
    // when PFIFO was already parked at a command boundary when the PUT advanced.
    return GET_MASK(dmaState, NV_PFIFO_CACHE1_DMA_STATE_ERROR) == NV_PFIFO_CACHE1_DMA_STATE_ERROR_NONE
        && GET_MASK(dmaState, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT) == 0
        && GET_MASK(dmaSubroutine, NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE) == 0;
}

bool nv2a_pfifo_consume_pending_kicks(NV2AState *d, bool *outHasMorePending,
	                                  bool *outReachedDrawBoundary,
	                                  bool *outSawNonConstantMethods,
                                      bool *outParseStateReliable,
                                      uint32_t *outFirstNonConstantMethod)
{
    bool found = false;
    LONG read = s_pendingDmaPutRead;
    LONG write = s_pendingDmaPutWrite;
    bool reached_draw_boundary = false;
    bool saw_non_constant_methods = false;
    bool parse_state_reliable = true;
    uint32_t first_non_constant_method = 0xFFFFFFFFu;
    bool have_parse_state = false;
    uint32_t dma_state = 0;
    uint32_t dma_subroutine = 0;
    uint32_t dma_const_load = NV2A_VERTEXSHADER_CONSTANTS;
    uint32_t expected_start = 0;

    if (outHasMorePending) {
        *outHasMorePending = false;
    }
    if (outReachedDrawBoundary) {
        *outReachedDrawBoundary = false;
    }
    if (outSawNonConstantMethods) {
        *outSawNonConstantMethods = false;
    }
    if (outParseStateReliable) {
        *outParseStateReliable = true;
    }
    if (outFirstNonConstantMethod) {
        *outFirstNonConstantMethod = 0xFFFFFFFFu;
    }

    memset(s_parsedSlotDirty, 0, sizeof(s_parsedSlotDirty));

    if (read == write) {
        return false;
    }

    while (read != write) {
        PendingDmaPutRange range = s_pendingDmaPutRanges[read];
        uint32_t consumed_end = range.end;
        bool range_reached_draw_boundary = false;
        bool range_saw_non_constant_methods = false;
        uint32_t range_first_non_constant_method = 0xFFFFFFFFu;

        if (!have_parse_state || range.start != expected_start) {
            if (!nv2a_pfifo_can_reseed_parse_state(
                range.start,
                range.dma_get,
                range.dma_state,
                range.dma_subroutine)) {
                parse_state_reliable = false;
                break;
            }
            dma_state = range.dma_state;
            dma_subroutine = range.dma_subroutine;
            dma_const_load = range.dma_const_load;
        }

        found |= nv2a_pfifo_extract_constants_inline_range(d, range.start, range.end,
                                                   &dma_state,
                                                   &dma_subroutine,
                                                   &dma_const_load,
	                                               &range_reached_draw_boundary,
	                                               &range_saw_non_constant_methods,
	                                               &range_first_non_constant_method,
	                                               &consumed_end);
        saw_non_constant_methods |= range_saw_non_constant_methods;
        if (first_non_constant_method == 0xFFFFFFFFu && range_first_non_constant_method != 0xFFFFFFFFu) {
            first_non_constant_method = range_first_non_constant_method;
        }
        have_parse_state = true;
        expected_start = consumed_end;

        if (consumed_end < range.end) {
            s_pendingDmaPutRanges[read].start = consumed_end;
            s_pendingDmaPutRanges[read].dma_get = consumed_end;
            s_pendingDmaPutRanges[read].dma_state = dma_state;
            s_pendingDmaPutRanges[read].dma_subroutine = dma_subroutine;
            s_pendingDmaPutRanges[read].dma_const_load = dma_const_load;
            reached_draw_boundary = range_reached_draw_boundary;
            break;
        }

        read = (read + 1) % NV2A_MAX_PENDING_DMA_PUT_RANGES;
        if (range_reached_draw_boundary) {
            reached_draw_boundary = true;
            break;
        }
    }

    if (outHasMorePending) {
        *outHasMorePending = (read != write);
    }
    if (outReachedDrawBoundary) {
        *outReachedDrawBoundary = reached_draw_boundary;
    }
    if (outSawNonConstantMethods) {
        *outSawNonConstantMethods = saw_non_constant_methods;
    }
    if (outParseStateReliable) {
        *outParseStateReliable = parse_state_reliable;
    }
    if (outFirstNonConstantMethod) {
        *outFirstNonConstantMethod = first_non_constant_method;
    }

    InterlockedExchange(&s_pendingDmaPutRead, read);
    return found;
}

void nv2a_pfifo_discard_pending_kicks(NV2AState *d)
{
	memset(s_parsedSlotDirty, 0, sizeof(s_parsedSlotDirty));

	// Before discarding, scan for SET_SEMAPHORE_OFFSET and
	// BACK_END_WRITE_SEMAPHORE_RELEASE commands and execute them.
	// Games (e.g. OutRun 2 SP) use GPU semaphore writes as fence counters;
	// discarding these causes infinite spin-waits.
	LONG read = s_pendingDmaPutRead;
	LONG write = s_pendingDmaPutWrite;

	if (read != write && d) {
		auto pg = &d->pgraph;

		// Map push buffer
		uint32_t *pf_regs = d->pfifo.regs;
		hwaddr dma_instance =
			GET_MASK(pf_regs[NV_PFIFO_CACHE1_DMA_INSTANCE],
			         NV_PFIFO_CACHE1_DMA_INSTANCE_ADDRESS_MASK) << 4;
		hwaddr dma_len;
		uint8_t *dma = (uint8_t*)nv_dma_map(d, dma_instance, &dma_len);

		if (dma) {
			while (read != write) {
				PendingDmaPutRange range = s_pendingDmaPutRanges[read];
				uint32_t dmaState = range.dma_state;
				uint32_t dmaSubroutine = range.dma_subroutine;

				uint32_t method_type =
					GET_MASK(dmaState, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE);
				uint32_t method =
					GET_MASK(dmaState, NV_PFIFO_CACHE1_DMA_STATE_METHOD) << 2;
				uint32_t method_count =
					GET_MASK(dmaState, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT);
				bool method_inc =
					(method_type == NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_INC);

				uint32_t sub_ret = dmaSubroutine & 0xfffffffc;
				bool sub_active =
					GET_MASK(dmaSubroutine, NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE) != 0;

				uint32_t pos = range.start;
				uint32_t end = range.end;

				for (int budget = 65536; budget > 0 && pos != end; --budget) {
					if (pos >= dma_len) break;

					uint32_t word = *(uint32_t*)(dma + pos);
					pos += 4;

					if (method_count) {
						if (method == NV097_SET_SEMAPHORE_OFFSET) {
							pg->regs[NV_PGRAPH_SEMAPHOREOFFSET] = word;
						}
						else if (method == NV097_BACK_END_WRITE_SEMAPHORE_RELEASE) {
							uint32_t semaphore_offset = pg->regs[NV_PGRAPH_SEMAPHOREOFFSET];
							xbox::addr_xt semaphore_dma_len;
							uint8_t *semaphore_data = (uint8_t*)nv_dma_map(d,
								pg->dma_semaphore, &semaphore_dma_len);
							if (semaphore_offset < semaphore_dma_len) {
								stl_le_p((uint32_t*)(semaphore_data + semaphore_offset), word);
							}
						}
						if (method_inc) method += 4;
						method_count--;
					} else {
						if ((word & 0xe0000003) == 0x20000000) {
							pos = word & 0x1fffffff;
						} else if ((word & 3) == 1) {
							pos = word & 0xfffffffc;
						} else if ((word & 3) == 2) {
							if (!sub_active) {
								sub_ret = pos;
								sub_active = true;
								pos = word & 0xfffffffc;
							}
						} else if (word == 0x00020000) {
							if (sub_active) {
								pos = sub_ret;
								sub_active = false;
							}
						} else if ((word & 0xe0030003) == 0) {
							method       = word & 0x1FFC;
							method_count = (word >> 18) & 0x7FF;
							method_inc   = true;
						} else if ((word & 0xe0030003) == 0x40000000) {
							method       = word & 0x1FFC;
							method_count = (word >> 18) & 0x7FF;
							method_inc   = false;
						}
					}
				}
				read = (read + 1) % NV2A_MAX_PENDING_DMA_PUT_RANGES;
			}
		}
	}

	InterlockedExchange(&s_pendingDmaPutRead, s_pendingDmaPutWrite);
}

void nv2a_pfifo_get_pending_dma_debug(uint32_t *outCurrentDepth,
                                      uint32_t *outHighWater,
                                      uint32_t *outOverflowCount)
{
    LONG read = s_pendingDmaPutRead;
    LONG write = s_pendingDmaPutWrite;

    if (outCurrentDepth) {
        *outCurrentDepth = (uint32_t)nv2a_pfifo_pending_dma_depth(read, write);
    }
    if (outHighWater) {
        *outHighWater = (uint32_t)s_pendingDmaPutHighWater;
    }
    if (outOverflowCount) {
        *outOverflowCount = (uint32_t)s_pendingDmaPutOverflowCount;
    }
}

void nv2a_pfifo_reset_pending_dma_debug()
{
    LONG read = s_pendingDmaPutRead;
    LONG write = s_pendingDmaPutWrite;
    InterlockedExchange(&s_pendingDmaPutHighWater, nv2a_pfifo_pending_dma_depth(read, write));
    InterlockedExchange(&s_pendingDmaPutOverflowCount, 0);
}
