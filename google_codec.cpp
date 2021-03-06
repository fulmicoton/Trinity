#include "google_codec.h"
#include "docidupdates.h"
#include <compress.h>
#include <memory>
#include <ansifmt.h>

#pragma mark ENCODER

void Trinity::Codecs::Google::Encoder::begin_term()
{
	auto out{&sess->indexOut};

	curBlockSize = 0;
	lastCommitedDocID = 0;
	prevBlockLastDocumentID = 0;
	hitsData.clear();
	termDocuments = 0;
	curTermOffset = out->size() + sess->indexOutFlushed;

	if (CONSTRUCT_SKIPLIST)
        {
                // 16bits for the number of skiplist entries, see end_term()
                out->RoomFor(sizeof(uint16_t));
        }
}

void Trinity::Codecs::Google::Encoder::begin_document(const docid_t documentID)
{
	require(documentID);
	if (unlikely(documentID <= lastCommitedDocID))
	{
		Print("Unexpected documentID(", documentID, ") <= lastCommitedDocID(", lastCommitedDocID, ")\n");
		std::abort();
	}

	curDocID = documentID;
	lastPos = 0;
	curPayloadSize = 0;
	blockFreqs[curBlockSize] = 0;
}

void Trinity::Codecs::Google::Encoder::new_hit(const uint32_t pos, const range_base<const uint8_t *, const uint8_t> payload) 
{
	static constexpr bool trace{false};
	const uint8_t payloadSize = payload.size();

	if (!pos && !payloadSize)
	{
		// this is perfectly valid
		return;
	}

	Drequire(payload.size() <= sizeof(uint64_t));	 // un-necessary?
	Drequire(pos < Limits::MaxPosition);
	Drequire(pos >= lastPos);

	const uint32_t delta = pos - lastPos;


	if (trace)
		SLog("HIT ", pos, " => ", delta, ", ", payload.size(), "\n");

	++blockFreqs[curBlockSize];

	if (TRACK_PAYLOADS)
	{
		if (payloadSize != curPayloadSize)
		{
			hitsData.encode_varbyte32((delta << 1) | 1);
			hitsData.pack(payloadSize);
			curPayloadSize = payloadSize;
		}
		else
		{
			hitsData.encode_varbyte32(delta << 1);
		}

		if (payloadSize)
			hitsData.serialize(payload.offset, payloadSize);
	}
	else
	{
		hitsData.encode_varbyte32(delta);
	}

	lastPos = pos;
}

void Trinity::Codecs::Google::Encoder::end_document()
{
	static constexpr bool trace{false};

	if (trace)
		SLog("end document ", curDocID, " ", lastCommitedDocID, " ", curBlockSize, "\n");

	docDeltas[curBlockSize++] = curDocID - lastCommitedDocID;
	if (curBlockSize == N)
		commit_block();

	lastCommitedDocID = curDocID;
	++termDocuments;
}

void Trinity::Codecs::Google::Encoder::end_term(term_index_ctx *tctx) 
{
	static constexpr bool trace{false};
	auto out{&sess->indexOut};

	if (curBlockSize)
		commit_block();

	if (trace)
		SLog("ENDING term ", curTermOffset, "\n");


	if (CONSTRUCT_SKIPLIST)
        {
                const uint16_t skipListEntries = skipListData.size() / (sizeof(docid_t) + sizeof(uint32_t));

		if (trace)
			SLog("Skiplist of size ", skipListEntries, "\n");

		require(skipListData.size() == skipListEntries * (sizeof(docid_t) + sizeof(uint32_t)));

                out->serialize(skipListData.data(), skipListData.size());     // actual skiplist
                *(uint16_t *)(out->data() + (curTermOffset - sess->indexOutFlushed)) = skipListEntries; // skiplist size in entries in the index chunk header
        }

        tctx->indexChunk.Set(curTermOffset, (out->size() + sess->indexOutFlushed) - curTermOffset);
        tctx->documents = termDocuments;

	skipListData.clear();
}

void Trinity::Codecs::Google::Encoder::commit_block()
{
        static constexpr bool trace{false};
        const auto delta = curDocID - prevBlockLastDocumentID;
        const auto n = curBlockSize - 1;
        auto out{&sess->indexOut};

        if (trace)
                SLog("Commiting block, curBlockSize = ", curBlockSize, ", curDocID = ", curDocID, ", prevBlockLastDocumentID = ", prevBlockLastDocumentID, ", delta = ", delta, "  ", out->size() + sess->indexOutFlushed, "\n");

        // build the new block
        block.clear();
        for (uint32_t i{0}; i != n; ++i) // exclude that last one because it will be referenced in the header
        {
                if (trace)
                        SLog("<< ", docDeltas[i], "\n");

                block.encode_varbyte32(docDeltas[i]);
        }

        for (uint32_t i{0}; i != curBlockSize; ++i)
        {
                if (trace)
                        SLog("<< freq ", blockFreqs[i], "\n");

                block.encode_varbyte32(blockFreqs[i]);
        }

        const auto blockLength = block.size() + hitsData.size();

        if (--skiplistEntryCountdown == 0)
        {
                if (trace)
                        SLog("NEW skiplist record for ", prevBlockLastDocumentID, ", so far: ", skipListData.size() / (sizeof(docid_t) + sizeof(uint32_t)), "\n");

		if (likely(skipListData.size() / (sizeof(docid_t) + sizeof(uint32_t)) < UINT16_MAX))
                {
                        // we can only support upto 65k skiplist entries so that
                        // we will only need a u16 to store that number in the index chunk header for the term
                        skipListData.pack(prevBlockLastDocumentID, uint32_t(out->size() - curTermOffset));

                        if (trace)
                                SLog("NOW skipListData.size = ", skipListData.size(), "\n");
                }
                skiplistEntryCountdown = SKIPLIST_STEP;
        }

        require(curBlockSize);

        out->encode_varbyte32(delta);       // delta to last docID in block from previous block's last document ID
        out->encode_varbyte32(blockLength); // block length in bytes, excluding this header
        out->pack(curBlockSize);            // one byte will suffice

        out->serialize(block.data(), block.size());
        out->serialize(hitsData.data(), hitsData.size());
        hitsData.clear();

        prevBlockLastDocumentID = curDocID;
        curBlockSize = 0;

        if (trace)
                SLog("Commited Block ", out->size() + sess->indexOutFlushed, "\n");
}

range32_t Trinity::Codecs::Google::IndexSession::append_index_chunk(const Trinity::Codecs::AccessProxy *src_, const term_index_ctx srcTCTX)
{
        auto src = static_cast<const Trinity::Codecs::Google::AccessProxy *>(src_);
        const auto o = indexOut.size() + indexOutFlushed;

        indexOut.serialize(src->indexPtr + srcTCTX.indexChunk.offset, srcTCTX.indexChunk.size());
        return {uint32_t(o), srcTCTX.indexChunk.size()};
}

void Trinity::Codecs::Google::IndexSession::merge(IndexSession::merge_participant *participants, const uint16_t participantsCnt, Trinity::Codecs::Encoder *encoder_)
{
        static constexpr bool trace{false};

        struct chunk final
        {
                const uint8_t *p;
                const uint8_t *e;
                masked_documents_registry *maskedDocsReg;

                struct
                {
                        docid_t documents[N];
                        uint32_t freqs[N];
                        uint8_t size;
                        uint8_t idx;
                } cur_block;

                constexpr size_t size() noexcept
                {
                        return e - p;
                }

                bool skip_current()
                {
                        static constexpr bool trace{false};
			uint8_t payloadSize{0};

                        if (trace)
                                SLog("Skipping current cur_block.idx = ", cur_block.idx, " out of ", cur_block.size, ", freq = ", cur_block.freqs[cur_block.idx], "\n");

                        for (auto n = cur_block.freqs[cur_block.idx]; n; --n)
                        {
                                uint32_t dummy;

                                varbyte_get32(p, dummy);
			
				if (TRACK_PAYLOADS)
				{
					if (dummy&1)
						payloadSize = *p++;

					p+=payloadSize;
				}
                        }

                        return ++cur_block.idx == cur_block.size;
                }
        };

        chunk chunks[participantsCnt];
        uint16_t toAdvance[participantsCnt];
        uint16_t rem{participantsCnt};
        auto encoder = static_cast<Trinity::Codecs::Google::Encoder *>(encoder_);

        for (uint32_t i{0}; i != participantsCnt; ++i)
        {
                auto c = chunks + i;

                c->p = participants[i].ap->indexPtr + participants[i].tctx.indexChunk.offset;
                c->e = c->p + participants[i].tctx.indexChunk.size();
                c->maskedDocsReg = participants[i].maskedDocsReg;

		if (participants[i].tctx.indexChunk.size())
		{
			require(c->p);
			require(c->e);
		}

		if (CONSTRUCT_SKIPLIST)
                {
                        // skip past the skiplist
                        auto p = c->p;
                        const auto skipListEntriesCnt = *(uint16_t *)p;
                        p += sizeof(uint16_t);

                        if (skipListEntriesCnt)
                                c->e = c->e - (skipListEntriesCnt * (sizeof(uint32_t) + sizeof(uint32_t)));

                        c->p = p;
                }

                // Simplifies refill()
                c->cur_block.size = 1;
                c->cur_block.documents[0] = 0;

                if (trace)
                        SLog("merge participant ", i, " ", participants[i].tctx.indexChunk, " ", ptr_repr(c->p), " ", ptr_repr(c->e), "\n");
        }

        const auto refill = [](auto *__restrict__ const c) {
                uint32_t _v;
                auto p = c->p;
                const auto prevBlockLastID = c->cur_block.documents[c->cur_block.size - 1];

                varbyte_get32(p, _v);

                const auto thisBlockLastDocID = prevBlockLastID + _v;
                uint32_t blockLength;

                varbyte_get32(p, blockLength);
		require(blockLength);

                const auto n = *p++;
                auto id{prevBlockLastID};
                const auto k = n - 1;

                if (trace)
                        SLog("Refilling chunk prevBlockLastID = ", prevBlockLastID, ", thisBlockLastDocID = ", thisBlockLastDocID, " => blockLength = ", blockLength, ", n = ", n, "\n");

		// sanity check
		if (unlikely(n > N))
		{
			Print("Unexpected n(", n, ") > N(", N, ")\n");
			std::abort();
		}
		require(blockLength);


                for (uint8_t i{0}; i != k; ++i)
                {
                        varbyte_get32(p, _v);
                        id += _v;

                        if (trace)
                                SLog("<< docID ", id, "\n");

                        c->cur_block.documents[i] = id;
                }

                c->cur_block.documents[k] = thisBlockLastDocID;

                for (uint8_t i{0}; i != n; ++i)
                {
                        varbyte_get32(p, _v);
                        c->cur_block.freqs[i] = _v;

                        if (trace)
                                SLog("<< freq(", c->cur_block.freqs[i], ")\n");
                }

                c->cur_block.size = n;
                c->cur_block.idx = 0;
                c->p = p;

                if (trace)
                        SLog("block size = ", c->cur_block.size, "\n");
        };

        const auto append_from = [encoder](auto c) {
                static constexpr bool trace{false};
                const auto idx = c->cur_block.idx;
                const auto did = c->cur_block.documents[idx];
                auto freq = c->cur_block.freqs[idx];
                auto p = c->p;
		uint8_t payloadSize{0};
		uint64_t payload;
		auto bytes = (uint8_t *)&payload;

                encoder->begin_document(did);

                if (trace)
                        SLog("APENDING document ", did, " freq ", freq, "\n");

                for (uint32_t i{0}, pos{0}; i != freq; ++i)
                {
                        uint32_t step;

                        varbyte_get32(p, step);

			if (TRACK_PAYLOADS)
			{
				if (step&1)
				{
					payloadSize = *p++;
					Dexpect(payloadSize <= sizeof(uint64_t));
				}

				if (payloadSize)
				{
					memcpy(bytes, p, payloadSize);
					p+=payloadSize;
				}

				pos += step>>1;
			}
			else
				pos+=step;

                        if (trace)
                                SLog("<< ", pos, "\n");

                        encoder->new_hit(pos, {bytes, payloadSize});
                }

                encoder->end_document();

                c->p = p;
                c->cur_block.freqs[idx] = 0; // this is important, otherwise skip_current() will skip those hits we just consumed
        };

        for (uint32_t i{0}; i != participantsCnt; ++i)
	{
		if (trace)
			SLog("Refilling ", i, " ", ptr_repr(chunks[i].p), "\n");

                refill(chunks + i);
	}

        for (;;)
        {
                uint32_t toAdvanceCnt = 1;
                docid_t lowestDID = chunks[0].cur_block.documents[chunks[0].cur_block.idx];

                toAdvance[0] = 0;
                for (uint32_t i{1}; i != rem; ++i)
                {
                        if (const auto id = chunks[i].cur_block.documents[chunks[i].cur_block.idx]; id < lowestDID)
                        {
                                lowestDID = id;
                                toAdvanceCnt = 1;
                                toAdvance[0] = i;
                        }
                        else if (id == lowestDID)
                        {
                                toAdvance[toAdvanceCnt++] = i;
                        }
                }

                // We use the first chunk
                auto maskedDocsReg = chunks[toAdvance[0]].maskedDocsReg;

                if (trace)
                        SLog("To advance ", toAdvanceCnt, " ", toAdvance[0], " ", lowestDID, "\n");

                if (!maskedDocsReg->test(lowestDID))
                {
                        const auto src = chunks + toAdvance[0]; // first is always the most recent

                        append_from(src);
                }
                else if (trace)
                        SLog("MASKED ", lowestDID, "\n");

                do
                {
                        auto idx = toAdvance[--toAdvanceCnt];
                        auto c = chunks + idx;

                        if (trace)
                                SLog("ADVANCING ", idx, "\n");

                        if (c->skip_current()) // end of the block
                        {
                                if (c->p != c->e)
                                {
                                        // more blocks available
                                        if (trace)
                                                SLog("No more block documents but more content in index chunk\n");

                                        refill(c);
                                }
                                else
                                {
                                        // exhausted
                                        if (--rem == 0)
                                        {
                                                // no more chunks to process
                                                if (trace)
                                                        SLog("No More Chunks\n");

                                                goto l1;
                                        }

                                        // We can't chunks[idx] = chunks[rem] because
                                        // of the invariant chunks[0] being the latest segments
                                        memmove(c, c + 1, (rem - idx) * sizeof(chunk));
                                }
                        }

                } while (toAdvanceCnt);
        }
l1:;
}

#pragma mark DECODER
uint32_t Trinity::Codecs::Google::Decoder::skiplist_search(const docid_t target) const noexcept
{
        // we store {previous block's last ID, block's offset}
        // in skiplist[], because when we unpack a block, we need to know
        // the previous block last document ID.
        //
        // So we need to use binary search to look for the last skiplist entry where
        // target > entry.first
        // We could use std::lower_bound() twice(if returned iterator points to an entry where entry.first == target)
        // but we 'll just roll out own here
        uint32_t idx{UINT32_MAX};

        for (int32_t top{int32_t(skiplist.size()) - 1}, btm{int32_t(skipListIdx)}; btm <= top;)
        {
                const auto mid = (btm + top) / 2;
                const auto v = skiplist[mid].first;

                if (target < v)
                        top = mid - 1;
                else
                {
                        if (v != target)
                                idx = mid;
                        else if (mid != skipListIdx)
                        {
                                // we need this
                                idx = mid - 1;
                        }

                        btm = mid + 1;
                }
        }

        return idx;
}

void Trinity::Codecs::Google::Decoder::skip_block_doc()
{
        static constexpr bool trace{false};
        // just advance to the next document in the current block
        // skip current document's hits/positions first

        if (trace)
                SLog("skipping document index ", blockDocIdx, ", freq = ", freqs[blockDocIdx], "\n");

	// we reset freqs[blockDocIdx] when we materialise so this works fine if we materialise_hits() and then attempt to skip_block_doc()
        const auto freq = freqs[blockDocIdx];
        uint8_t curPayloadSize{0};
        uint32_t dummy;

        for (uint32_t i{0}; i != freq; ++i)
        {
                varbyte_get32(p, dummy);

		if (TRACK_PAYLOADS)
                {
                        if (dummy & 1)
                        {
                                // new payload size
                                curPayloadSize = *p++;
                        }

                        p += curPayloadSize;
                }
        }

        // p now points to the positions/attributes for the current document
        // current document is documents[blockDocIdx]
        // and its frequency is freqs[blockDocIdx]
        // You can access the current document at documents[blockDocIdx], freq at freqs[blockDocIdx] and you can materialize
        // the document attributes with materialize_attributes()
}

void Trinity::Codecs::Google::Decoder::materialize_hits(const exec_term_id_t termID, DocWordsSpace *dwspace, term_hit *out)
{
	static constexpr bool trace{false};
        const auto freq = freqs[blockDocIdx];
        tokenpos_t pos{0};
        uint8_t curPayloadSize{0};
        uint64_t payload{0};
        auto *const bytes = (uint8_t *)&payload;
        uint32_t step;

	if (trace)
		SLog("Materializing ", freq, " hits\n");

        for (tokenpos_t i{0}; i != freq; ++i)
        {
                varbyte_get32(p, step);

                if (TRACK_PAYLOADS)
                {
                        if (step & 1)
                        {
                                // new payload size
                                curPayloadSize = *p++;

				if (trace)
					SLog("Payload size = ", curPayloadSize, "\n");

				Dexpect(curPayloadSize <= sizeof(uint64_t));	 // XXX: un-necessary check
                        }

                        pos += step >> 1;

                        if (curPayloadSize)
                        {
                                memcpy(bytes, p, curPayloadSize);
                                p += curPayloadSize;
                        }
                        else
                                payload = 0;
                }
                else
                        pos += step;

		if (trace)
			SLog("Pos = ", pos, "\n");

		if (pos)
		{
	 		// pos == 0  if this not e.g a title or body match but e.g a special token
			// set during indexing e.g site:www.google.com
			// you could of course use a different position for that purprose (i.e a very large position, that is guaranteed
			// to not match any terms in the document), but 0 makes sense
                	dwspace->set(termID, pos);
		}

                out[i] = {payload, pos, curPayloadSize};
        }

        // reset explicitly
        // we have already materialized here
        // This is also important because otherwise next() and skip() would haywire (see skip_block_doc() )
        freqs[blockDocIdx] = 0;
}

void Trinity::Codecs::Google::Decoder::unpack_block(const docid_t thisBlockLastDocID, const uint8_t n)
{
        static constexpr bool trace{false};
        const auto k{n - 1};
        auto id{blockLastDocID};

        if (trace)
                SLog("Now unpacking block contents, n = ", n, ", blockLastDocID = ", blockLastDocID, ", thisBlockLastDocID = ", thisBlockLastDocID, "\n");
        require(n <= N);

        for (uint8_t i{0}; i != k; ++i)
        {
                uint32_t delta;

                varbyte_get32(p, delta);
                id += delta;

                if (trace)
                        SLog("<< ", id, "\n");

                documents[i] = id;

                expect(id < thisBlockLastDocID);
        }

        for (uint32_t i{0}; i != n; ++i)
        {
                uint32_t v;

                varbyte_get32(p, v);
                freqs[i] = v;

                if (trace)
                        SLog("Freq ", i, " ", freqs[i], "\n");
        }

        blockLastDocID = thisBlockLastDocID;
        documents[k] = blockLastDocID;

        // We don't need to track current block documents cnt, because
        // we can just check if (documents[blockDocIdx] == blockLastDocID)
        blockDocIdx = 0;
}

void Trinity::Codecs::Google::Decoder::seek_block(const docid_t target)
{
        static constexpr bool trace{false};

        if (trace)
                SLog("SEEKING ", target, "\n");

        for (;;)
        {
                uint32_t _v;

                varbyte_get32(p, _v);

                const auto thisBlockLastDocID = blockLastDocID + _v;
                uint32_t blockSize;

                varbyte_get32(p, blockSize);

                const auto blockDocsCnt = *p++;

                if (trace)
                        SLog("thisBlockLastDocID = ", thisBlockLastDocID, ", blockSize = ", blockSize, ", blockDocsCnt, ", blockDocsCnt, "\n");

		Drequire(blockDocsCnt <= N);

                if (target > thisBlockLastDocID)
                {
                        if (trace)
                                SLog("Target(", target, ") past this block (thisBlockLastDocID = ", thisBlockLastDocID, ")\n");

                        p += blockSize;

                        if (p == chunkEnd)
                        {
                                // exchausted all blocks
                                if (trace)
                                        SLog("Finalizing\n");

                                finalize();
                                return;
                        }

                        blockLastDocID = thisBlockLastDocID;

                        if (trace)
                                SLog("Skipped past block\n");
                }
                else
                {
                        if (trace)
                                SLog("Found potential block\n");

                        unpack_block(thisBlockLastDocID, blockDocsCnt);
                        break;
                }
        }
}

void Trinity::Codecs::Google::Decoder::unpack_next_block()
{
        static constexpr bool trace{false};
        uint32_t _v;

        varbyte_get32(p, _v);

        const auto thisBlockLastDocID = blockLastDocID + _v;
        uint32_t blockSize;

        varbyte_get32(p, blockSize);

	require(blockSize);

        const auto blockDocsCnt = *p++;

	require(blockDocsCnt <= N);

        if (trace)
                SLog("UNPACKING next block, thisBlockLastDocID = ", thisBlockLastDocID, ", blockSize = ", blockSize, ", blockDocsCnt = ", blockDocsCnt, ", blockLastDocID = ", blockLastDocID, "\n");

        unpack_block(thisBlockLastDocID, blockDocsCnt);
}

void Trinity::Codecs::Google::Decoder::skip_remaining_block_documents()
{
        static constexpr bool trace{false};

        if (trace)
                SLog("Skipping current block (blockLastDocID = ", blockLastDocID, ")\n");

        for (;;)
        {
                auto freq = freqs[blockDocIdx];
                uint32_t dummy;
		uint8_t payloadSize{0};

                if (trace)
                        SLog("Skipping ", documents[blockDocIdx], " ", freq, "\n");

                while (freq)
                {
                        --freq;
                        varbyte_get32(p, dummy);

			if (TRACK_PAYLOADS)
                        {
                                if (dummy & 1)
                                        payloadSize = *p++;

                                p += payloadSize;
                        }
                }

                if (documents[blockDocIdx] == blockLastDocID)
                        break;
                else
                        ++blockDocIdx;
        }
}

Trinity::docid_t Trinity::Codecs::Google::Decoder::begin()
{
        static constexpr bool trace{false};

        if (trace)
                SLog("Resetting\n");

        if (p != chunkEnd)
        {
                unpack_next_block();
        }
        else
        {
                // ODD, not a single document for this term
                // doesn't make much sense, but we can handle it
                finalize();
        }

        curDocument.id = documents[blockDocIdx];
        curDocument.freq = freqs[blockDocIdx];

        return documents[blockDocIdx];
}

bool Trinity::Codecs::Google::Decoder::next()
{
        static constexpr bool trace{false};

        if (trace)
                SLog("NEXT blockDocIdx = ", blockDocIdx, " [", documents[blockDocIdx], "] blockLastDocID = ", blockLastDocID, "]\n");

        if (documents[blockDocIdx] == blockLastDocID)
        {
                if (trace)
                        SLog("done with block\n");

                skip_block_doc();
                if (p != chunkEnd)
                {
                        // we are at the last document in the block
                        if (trace)
                                SLog("Yes, have more blocks\n");

                        // more blocks available
                        unpack_next_block();
                }
                else
                {
                        // exhausted all documents

                        if (trace)
                                SLog("Exhausted all documents\n");

                        finalize();
                        return false;
                }
        }
        else
        {
                if (trace)
                        SLog("Just skipping block\n");

                skip_block_doc();
		++blockDocIdx;
        }

        curDocument.id = documents[blockDocIdx];
        curDocument.freq = freqs[blockDocIdx];

        return true;
}

bool Trinity::Codecs::Google::Decoder::seek(const docid_t target)
{
        static constexpr bool trace{false};

        if (trace)
                SLog(ansifmt::bold, ansifmt::color_green, "SKIPPING to ", target, ansifmt::reset, ", currently at ", documents[blockDocIdx], ", blockLastDocID = ", blockLastDocID, "\n");

        if (target > blockLastDocID)
        {
                // we can safely assume (p != chunkEnd)
                // because in that case we 'd have finalize() and
                // blockLastDocID would have been MaxDocIDValue
                // and (target > blockLastDocID) would have been false
                skip_remaining_block_documents();

                if (unlikely(p == chunkEnd))
                {
                        if (trace)
                                SLog("Exhausted documents\n");

                        finalize();
                        return false;
                }

                if (trace)
                        SLog("Skipped remaining block documents, skipListIdx = ", skipListIdx, " ", skiplist.size(), "\n");

                if (skipListIdx != skiplist.size())
                {
                        const auto idx = skiplist_search(target);

                        if (trace)
                        {
                                SLog("idx = ", idx, ", target = ", target, " ", idx, "\n");

                                for (uint32_t i{0}; i != skiplist.size(); ++i)
                                {
                                        const auto &it = skiplist[i];

                                        Print(i, " ", it, ": ", target, "\n");
                                }
                        }

                        if (idx != UINT32_MAX)
                        {
                                // there is a skiplist entry we can use
				const auto savedBlockLastDocID = blockLastDocID;

                                blockLastDocID = skiplist[idx].first;
                                p = base + skiplist[idx].second;

                                if (trace)
                                        SLog("Skipping ahead to past ", blockLastDocID, "  target = ", target, ", savedBlockLastDocID = ", savedBlockLastDocID, "\n");

				if (target > savedBlockLastDocID)
				{
					// skip _past_ if (target > previous blockLastDocID)
                                	skipListIdx = idx + 1;
				}
                        }
                }

                seek_block(target);
        }

        // If it's anywhere, it must be in this current block
        for (;;)
        {
                const auto docID = documents[blockDocIdx];

                if (trace)
                        SLog("Scannning current block blockDocIdx = ", blockDocIdx, ", docID = ", docID, "\n");

                if (docID > target)
                {
                        if (trace)
                                SLog("Not in this block or maybe any block\n");
                        break;
                }
                else if (docID == target)
                {
                        // got it
                        if (trace)
                                SLog("Got target\n");
                        curDocument.id = documents[blockDocIdx];
                        curDocument.freq = freqs[blockDocIdx];
                        return true;
                }
                else if (docID == blockLastDocID)
                {
                        // exhausted block documents and still not here
                        // we determined we don't have this document
                        if (trace)
                                SLog("Exhausted block\n");

                        break;
                }
                else
                {
                        if (trace)
                                SLog("Skipping block document\n");

                        skip_block_doc();
			++blockDocIdx;
                }
        }

        curDocument.id = documents[blockDocIdx];
        curDocument.freq = freqs[blockDocIdx];
        return false;
}

void Trinity::Codecs::Google::Decoder::init(const term_index_ctx &tctx, Trinity::Codecs::AccessProxy *proxy)
{
        static constexpr bool trace{false};
        [[maybe_unused]] auto access = static_cast<Trinity::Codecs::Google::AccessProxy *>(proxy);
        auto indexPtr = access->indexPtr;
        auto ptr = indexPtr + tctx.indexChunk.offset;
        const auto chunkSize = tctx.indexChunk.size();

        chunkEnd = ptr + chunkSize;
	p = base = ptr;
        blockLastDocID = 0;
        blockDocIdx = 0;
        documents[0] = 0;
        freqs[0] = 0;
        skipListIdx = 0;

	if (trace)
		SLog(ansifmt::bold, "initializing decoder", ansifmt::reset, "\n");

        if (unlikely(!chunkSize))
                finalize();
	else if (CONSTRUCT_SKIPLIST)
        {
                const auto skipListEntriesCnt = *(uint16_t *)ptr;
                ptr += sizeof(uint16_t);

                p = ptr; // p points past the u16 that holds total number of skiplist entries

		if (trace)
			SLog("skipListEntriesCnt = ", skipListEntriesCnt, "\n");

                if (skipListEntriesCnt)
                {
                        const auto skiplistData = (base + chunkSize) - (skipListEntriesCnt * (sizeof(uint32_t) + sizeof(uint32_t)));
                        const auto *it = skiplistData;
                        // That many skiplist entries(deterministic)
                        // UPDATE: we already track that count in the header anyway
                        //const auto n = ((tctx.documents + (N - 1)) / N) / SKIPLIST_STEP;

                        for (uint32_t i{0}; i != skipListEntriesCnt; ++i)
                        {
                                const auto id = *(docid_t *)it;
                                it += sizeof(docid_t);
                                const auto offset = *(uint32_t *)it;
                                it += sizeof(uint32_t);

				if (trace)
					SLog("skiplist (", id, ", ", offset, ")\n");

				if (skiplist.size())
					require(id > skiplist.back().first);

                                skiplist.push_back({id, offset});
                        }

                        if (trace)
                                SLog(skiplist.size(), " skiplist entries\n");

                        chunkEnd = skiplistData; // end chunk before the skiplist
                }
		else if (trace)
			SLog("No skiplist entries\n");
        }
}

Trinity::Codecs::Decoder *Trinity::Codecs::Google::AccessProxy::new_decoder(const term_index_ctx &tctx)
{
        auto d = std::make_unique<Trinity::Codecs::Google::Decoder>();

        d->init(tctx, this);
        return d.release();
}

void Trinity::Codecs::Google::IndexSession::begin()
{
}

void Trinity::Codecs::Google::IndexSession::end()
{
}

Trinity::Codecs::Encoder *Trinity::Codecs::Google::IndexSession::new_encoder()
{
        return new Trinity::Codecs::Google::Encoder(this);
}
