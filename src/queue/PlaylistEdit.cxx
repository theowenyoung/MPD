/*
 * Copyright 2003-2022 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Functions for editing the playlist (adding, removing, reordering
 * songs in the queue).
 *
 */

#include "Playlist.hxx"
#include "Listener.hxx"
#include "PlaylistError.hxx"
#include "player/Control.hxx"
#include "protocol/RangeArg.hxx"
#include "song/DetachedSong.hxx"
#include "SongLoader.hxx"

#include <stdlib.h>

void
playlist::OnModified() noexcept
{
	if (bulk_edit) {
		/* postponed to CommitBulk() */
		bulk_modified = true;
		return;
	}

	queue.IncrementVersion();

	listener.OnQueueModified();
}

void
playlist::Clear(PlayerControl &pc) noexcept
{
	Stop(pc);

	queue.Clear();
	current = -1;

	OnModified();
}

void
playlist::BeginBulk() noexcept
{
	assert(!bulk_edit);

	bulk_edit = true;
	bulk_modified = false;
}

void
playlist::CommitBulk(PlayerControl &pc) noexcept
{
	assert(bulk_edit);

	bulk_edit = false;
	if (!bulk_modified)
		return;

	if (queued < 0)
		/* if no song was queued, UpdateQueuedSong() is being
		   ignored in "bulk" edit mode; now that we have
		   shuffled all new songs, we can pick a random one
		   (instead of always picking the first one that was
		   added) */
		UpdateQueuedSong(pc, nullptr);

	OnModified();
}

unsigned
playlist::AppendSong(PlayerControl &pc, DetachedSong &&song)
{
	unsigned id;

	if (queue.IsFull())
		throw PlaylistError(PlaylistResult::TOO_LARGE,
				    "Playlist is too large");

	const DetachedSong *const queued_song = GetQueuedSong();

	id = queue.Append(std::move(song), 0);

	if (queue.random) {
		/* shuffle the new song into the list of remaining
		   songs to play */

		unsigned start;
		if (queued >= 0)
			start = queued + 1;
		else
			start = current + 1;
		if (start < queue.GetLength())
			queue.ShuffleOrderLastWithPriority(start, queue.GetLength());
	}

	UpdateQueuedSong(pc, queued_song);
	OnModified();

	return id;
}

unsigned
playlist::AppendURI(PlayerControl &pc, const SongLoader &loader,
		    const char *uri)
{
	return AppendSong(pc, loader.LoadSong(uri));
}

void
playlist::SwapPositions(PlayerControl &pc, unsigned song1, unsigned song2)
{
	if (!queue.IsValidPosition(song1) || !queue.IsValidPosition(song2))
		throw PlaylistError::BadRange();

	const DetachedSong *const queued_song = GetQueuedSong();

	queue.SwapPositions(song1, song2);

	if (queue.random) {
		/* update the queue order, so that current
		   still points to the current song order */

		queue.SwapOrders(queue.PositionToOrder(song1),
				 queue.PositionToOrder(song2));
	} else if (current >= 0){
		/* correct the "current" song order */

		if (unsigned(current) == song1)
			current = song2;
		else if (unsigned(current) == song2)
			current = song1;
	}

	UpdateQueuedSong(pc, queued_song);
	OnModified();
}

void
playlist::SwapIds(PlayerControl &pc, unsigned id1, unsigned id2)
{
	int song1 = queue.IdToPosition(id1);
	int song2 = queue.IdToPosition(id2);

	if (song1 < 0 || song2 < 0)
		throw PlaylistError::NoSuchSong();

	SwapPositions(pc, song1, song2);
}

void
playlist::SetPriorityRange(PlayerControl &pc,
			   RangeArg range,
			   uint8_t priority)
{
	if (!range.CheckClip(GetLength()))
		throw PlaylistError::BadRange();

	if (range.IsEmpty())
		return;

	/* remember "current" and "queued" */

	const int current_position = GetCurrentPosition();
	const DetachedSong *const queued_song = GetQueuedSong();

	/* apply the priority changes */

	queue.SetPriorityRange(range.start, range.end, priority, current);

	/* restore "current" and choose a new "queued" */

	if (current_position >= 0)
		current = queue.PositionToOrder(current_position);

	UpdateQueuedSong(pc, queued_song);
	OnModified();
}

void
playlist::SetPriorityId(PlayerControl &pc,
			unsigned song_id, uint8_t priority)
{
	int song_position = queue.IdToPosition(song_id);
	if (song_position < 0)
		throw PlaylistError::NoSuchSong();

	SetPriorityRange(pc, {unsigned(song_position), song_position + 1U}, priority);
}

void
playlist::DeleteInternal(PlayerControl &pc,
			 unsigned song, const DetachedSong **queued_p) noexcept
{
	assert(song < GetLength());

	unsigned songOrder = queue.PositionToOrder(song);

	if (playing && current == (int)songOrder) {
		const bool paused = pc.GetState() == PlayerState::PAUSE;

		/* the current song is going to be deleted: see which
		   song is going to be played instead */

		current = queue.GetNextOrder(current);
		if (current == (int)songOrder)
			current = -1;

		if (current >= 0 && !paused)
			/* play the song after the deleted one */
			try {
				PlayOrder(pc, current);
			} catch (...) {
				/* TODO: log error? */
			}
		else {
			/* stop the player */

			pc.LockStop();
			playing = false;
		}

		*queued_p = nullptr;
	} else if (current == (int)songOrder)
		/* there's a "current song" but we're not playing
		   currently - clear "current" */
		current = -1;

	/* now do it: remove the song */

	queue.DeletePosition(song);

	/* update the "current" and "queued" variables */

	if (current > (int)songOrder)
		current--;
}

void
playlist::DeletePosition(PlayerControl &pc, unsigned song)
{
	if (song >= queue.GetLength())
		throw PlaylistError::BadRange();

	const DetachedSong *queued_song = GetQueuedSong();

	DeleteInternal(pc, song, &queued_song);

	UpdateQueuedSong(pc, queued_song);
	OnModified();
}

void
playlist::DeleteRange(PlayerControl &pc, RangeArg range)
{
	if (!range.CheckClip(GetLength()))
		throw PlaylistError::BadRange();

	if (range.IsEmpty())
		return;

	const DetachedSong *queued_song = GetQueuedSong();

	do {
		DeleteInternal(pc, --range.end, &queued_song);
	} while (range.end != range.start);

	UpdateQueuedSong(pc, queued_song);
	OnModified();
}

void
playlist::DeleteId(PlayerControl &pc, unsigned id)
{
	int song = queue.IdToPosition(id);
	if (song < 0)
		throw PlaylistError::NoSuchSong();

	DeletePosition(pc, song);
}

void
playlist::StaleSong(PlayerControl &pc, const char *uri) noexcept
{
	/* don't remove the song if it's currently being played, to
	   avoid disrupting playback; a deleted file may still be
	   played if it's still open */
	// TODO: mark the song as "stale" and postpone deletion
	int current_position = playing
		? GetCurrentPosition()
		: -1;

	for (int i = queue.GetLength() - 1; i >= 0; --i)
		if (i != current_position && queue.Get(i).IsURI(uri))
			DeletePosition(pc, i);
}

void
playlist::MoveRange(PlayerControl &pc, RangeArg range, unsigned to)
{
	if (!queue.IsValidPosition(range.start) ||
	    !queue.IsValidPosition(range.end - 1))
		throw PlaylistError::BadRange();

	if (to + range.Count() - 1 >= GetLength())
		throw PlaylistError::BadRange();

	if (range.start == to)
		/* nothing happens */
		return;

	const DetachedSong *const queued_song = GetQueuedSong();

	queue.MoveRange(range.start, range.end, to);

	if (!queue.random && current >= 0) {
		/* update current */
		if (range.Contains(current))
			current += to - range.start;
		else if (unsigned(current) >= range.end &&
			 unsigned(current) <= to)
			current -= range.Count();
		else if (unsigned(current) >= to &&
			 unsigned(current) < range.start)
			current += range.Count();
	}

	UpdateQueuedSong(pc, queued_song);
	OnModified();
}

void
playlist::Shuffle(PlayerControl &pc, RangeArg range)
{
	if (!range.CheckClip(GetLength()))
		throw PlaylistError::BadRange();

	if (!range.HasAtLeast(2))
		/* needs at least two entries. */
		return;

	const DetachedSong *const queued_song = GetQueuedSong();
	if (playing && current >= 0) {
		unsigned current_position = queue.OrderToPosition(current);

		if (range.Contains(current_position)) {
			/* put current playing song first */
			queue.SwapPositions(range.start, current_position);

			if (queue.random) {
				current = queue.PositionToOrder(range.start);
			} else
				current = range.start;

			/* start shuffle after the current song */
			range.start++;
		}
	} else {
		/* no playback currently: reset current */

		current = -1;
	}

	queue.ShuffleRange(range.start, range.end);

	UpdateQueuedSong(pc, queued_song);
	OnModified();
}

void
playlist::SetSongIdRange(PlayerControl &pc, unsigned id,
			 SongTime start, SongTime end)
{
	assert(end.IsZero() || start < end);

	int position = queue.IdToPosition(id);
	if (position < 0)
		throw PlaylistError::NoSuchSong();

	bool was_queued = false;

	if (playing) {
		if (position == current)
			throw PlaylistError(PlaylistResult::DENIED,
					    "Cannot edit the current song");

		if (position == queued) {
			/* if we're manipulating the "queued" song,
			   the decoder thread may be decoding it
			   already; cancel that */
			pc.LockCancel();
			queued = -1;

			/* schedule a call to UpdateQueuedSong() to
			   re-queue the song with its new range */
			was_queued = true;
		}
	}

	DetachedSong &song = queue.Get(position);

	const auto duration = song.GetTag().duration;
	if (!duration.IsNegative()) {
		/* validate the offsets */

		if (start > duration)
			throw PlaylistError(PlaylistResult::BAD_RANGE,
					    "Invalid start offset");

		if (end >= duration)
			end = SongTime::zero();
	}

	/* edit it */
	song.SetStartTime(start);
	song.SetEndTime(end);

	/* announce the change to all interested subsystems */
	if (was_queued)
		UpdateQueuedSong(pc, nullptr);
	queue.ModifyAtPosition(position);
	OnModified();
}
