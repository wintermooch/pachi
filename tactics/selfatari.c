#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define QUICK_BOARD_CODE

#define DEBUG
#include "board.h"
#include "debug.h"
#include "mq.h"
#include "random.h"
#include "tactics/1lib.h"
#include "tactics/selfatari.h"
#include "tactics/dragon.h"
#include "nakade.h"


struct selfatari_state {
	int groupcts[S_MAX];
	group_t groupids[S_MAX][4];
	coord_t groupneis[S_MAX][4];
	int ataris;     /* Number of groups we put in atari with this move */
	int captures;	/* Number of groups captured */

	/* This is set if this move puts a group out of _all_
	 * liberties; we need to watch out for snapback then. */
	bool friend_has_no_libs;
	/* We may have one liberty, but be looking for one more.
	 * In that case, @needs_more_lib is id of group
	 * already providing one, don't consider it again. */
	group_t needs_more_lib;
	/* ID of the first liberty, providing it again is not
	 * interesting. */
	coord_t needs_more_lib_except;
};

static bool
three_liberty_suicide(struct board *b, group_t g, enum stone color, coord_t to, struct selfatari_state *s)
{
	/* If a group has three liberties, by playing on one of
	 * them it is possible to kill the group clumsily. Check
	 * against that condition: "After our move, the opponent
	 * can unconditionally capture the group."
	 *
	 * Examples:
	 *
	 * O O O O O O O   X X O O O O O O     v-v- ladder
	 * O X X X X X O   . O X X X X X O   . . . O O
	 * O X ! . ! X O   . O X ! . ! O .   O X X . O
	 * O X X X X X O   # # # # # # # #   O O O O O */

	/* Extract the other two liberties. */
	coord_t other_libs[2];
	bool other_libs_adj[2];
	for (int i = 0, j = 0; i < 3; i++) {
		coord_t lib = board_group_info(b, g).lib[i];
		if (lib != to) {
			other_libs_adj[j] = coord_is_adjecent(lib, to, b);
			other_libs[j++] = lib;
		}
	}

	/* Make sure this move is not useful by gaining liberties,
	 * splitting the other two liberties (quite possibly splitting
	 * 3-eyespace!) or connecting to a different group. */
	if (immediate_liberty_count(b, to) - (other_libs_adj[0] || other_libs_adj[1]) > 0)
		return false;
	assert(!(other_libs_adj[0] && other_libs_adj[1]));
	if (s->groupcts[color] > 1)
		return false;

	/* Playing on the third liberty might be useful if it enables
	 * capturing some group (are we doing nakade or semeai?). */
	for (int i = 0; i < s->groupcts[stone_other(color)]; i++)
		if (board_group_info(b, s->groupids[stone_other(color)][i]).libs <= 3)
			return false;


	/* Okay. This looks like a pretty dangerous situation. The
	 * move looks useless, it definitely converts us to a 2-lib
	 * group. But we still want to play it e.g. if it takes off
	 * liberties of some unconspicous enemy group, and of course
	 * also at the game end to leave just single-point eyes. */

	if (DEBUGL(6))
		fprintf(stderr, "3-lib danger\n");

	/* Therefore, the final suicidal test is: (After filling this
	 * liberty,) when opponent fills liberty [0], playing liberty
	 * [1] will not help the group, or vice versa. */
	bool other_libs_neighbors = coord_is_adjecent(other_libs[0], other_libs[1], b);
	for (int i = 0; i < 2; i++) {
		int null_libs = other_libs_neighbors + other_libs_adj[i];
		if (board_is_one_point_eye(b, other_libs[1 - i], color)) {
			/* The other liberty is an eye, happily go ahead.
			 * There are of course situations where this will
			 * take off semeai liberties, but without this check,
			 * many terminal endgame plays will be messed up. */
			return false;
		}
		if (immediate_liberty_count(b, other_libs[i]) - null_libs > 1) {
			/* Gains liberties. */
			/* TODO: Check for ladder! */
next_lib:
			continue;
		}
		foreach_neighbor(b, other_libs[i], {
			if (board_at(b, c) == color
			    && group_at(b, c) != g
			    && board_group_info(b, group_at(b, c)).libs > 1) {
				/* Can connect to a friend. */
				/* TODO: > 2? But maybe the group can capture
				 * a neighbor! But then better let it do that
				 * first? */
				goto next_lib;
			}
		});
		/* If we can capture a neighbor, better do it now
		 * before wasting a liberty. So no need to check. */
		/* Ok, the last liberty has no way to get out. */
		if (DEBUGL(6))
			fprintf(stderr, "3-lib dangerous: %s\n", coord2sstr(other_libs[i], b));
		return true;
	}

	return false;
}

static int
examine_friendly_groups(struct board *b, enum stone color, coord_t to, struct selfatari_state *s, int flags)
{
	for (int i = 0; i < s->groupcts[color]; i++) {
		/* We can escape by connecting to this group if it's
		 * not in atari. */
		group_t g = s->groupids[color][i];

		if (board_group_info(b, g).libs == 1) {
			if (!s->needs_more_lib)
				s->friend_has_no_libs = true;
			// or we already have a friend with 1 lib
			continue;
		}
		
		if (board_group_info(b, g).libs > 2) {
			/* Could we self-atari the group here? */
			if (flags & SELFATARI_3LIB_SUICIDE &&
			    board_group_info(b, g).libs == 3
			    && three_liberty_suicide(b, g, color, to, s))
				return true;
			return false;
		}

		/* We need to have another liberty, and
		 * it must not be the other liberty of
		 * the group. */
		int lib2 = board_group_other_lib(b, g, to);
		/* Maybe we already looked at another
		 * group providing one liberty? */
		if (s->needs_more_lib && s->needs_more_lib != g
		    && s->needs_more_lib_except != lib2)
			return false;

		/* Can we get the liberty locally? */
		/* Yes if we are route to more liberties... */
		if (s->groupcts[S_NONE] > 1)
			return false;
		/* ...or one liberty, but not lib2. */
		if (s->groupcts[S_NONE] > 0
		    && !coord_is_adjecent(lib2, to, b))
			return false;

		/* ...ok, then we can still contribute a liberty
		 * later by capturing something. */
		s->needs_more_lib = g;
		s->needs_more_lib_except = lib2;
		s->friend_has_no_libs = false;
	}

	return -1;
}

static int
examine_enemy_groups(struct board *b, enum stone color, coord_t to, struct selfatari_state *s)
{
	/* We may be able to gain a liberty by capturing this group. */
	group_t can_capture = 0;

	/* Examine enemy groups: */
	for (int i = 0; i < s->groupcts[stone_other(color)]; i++) {
		/* We can escape by capturing this group if it's in atari. */
		group_t g = s->groupids[stone_other(color)][i];
		if (board_group_info(b, g).libs == 2)
			s->ataris++;
		if (board_group_info(b, g).libs > 1)
			continue;

		s->captures++;
		/* But we need to get to at least two liberties by this;
		 * we already have one outside liberty, or the group is
		 * more than 1 stone (in that case, capturing is always
		 * nice!). */
		if (s->groupcts[S_NONE] > 0 || !group_is_onestone(b, g))
			return false;
		/* ...or, it's a ko stone, */
		if (neighbor_count_at(b, g, color) + neighbor_count_at(b, g, S_OFFBOARD) == 3) {
			/* and we don't have a group to save: then, just taking
			 * single stone means snapback! */
			if (!s->friend_has_no_libs)
				return false;
		}
		/* ...or, we already have one indirect liberty provided
		 * by another group. */
		if (s->needs_more_lib || (can_capture && can_capture != g))
			return false;
		can_capture = g;

	}

	if (DEBUGL(6))
		fprintf(stderr, "no cap group\n");

	if (!s->needs_more_lib && !can_capture && !s->groupcts[S_NONE]) {
		/* We have no hope for more fancy tactics - this move is simply
		 * a suicide, not even a self-atari. */
		if (DEBUGL(6))
			fprintf(stderr, "suicide\n");
		return true;
	}
	/* XXX: I wonder if it makes sense to continue if we actually
	 * just !s->needs_more_lib. */

	return -1;
}

static inline bool
is_neighbor_group(struct board *b, enum stone color, group_t g, struct selfatari_state *s)
{
	for (int i = 0; i < s->groupcts[color]; i++)
		if (g == s->groupids[color][i])
			return true;
	return false;
}


/* Instead of playing this self-atari, could we have connected/escaped by 
 * playing on the other liberty of a neighboring group ? */
static inline bool
is_bad_nakade(struct board *b, enum stone color, coord_t to, coord_t lib2, struct selfatari_state *s)
{
	/* Let's look at neighbors of the other liberty: */
	foreach_neighbor(b, lib2, {
		/* If the other liberty has empty neighbor,
		 * it must be the original liberty; otherwise,
		 * since the whole group has only 2 liberties,
		 * the other liberty may not be internal and
		 * we are nakade'ing eyeless group from outside,
		 * which is stupid. */
		if (board_at(b, c) == S_NONE) {
			if (c == to)
				continue;
			else
				return true;
		}});

	/* Let's look at neighbors of the other liberty: */
	foreach_neighbor(b, lib2, {
		if (board_at(b, c) != color)
			continue;

		group_t g2 = group_at(b, c);
		/* Looking for a group we don't know about */
		if (is_neighbor_group(b, color, g2, s))
			continue;
		
		/* Should connect these groups instead of self-atari on the other side. */
		return true;
	});

	return false;
}


/* Instead of playing this self-atari, could we have connected/escaped by 
 * playing on the other liberty of a neighboring group ? */
static inline bool
can_escape_instead(struct board *b, enum stone color, coord_t to, struct selfatari_state *s)
{
	for (int i = 0; i < s->groupcts[color]; i++) {
		group_t g = s->groupids[color][i];
		if (board_group_info(b, g).libs != 2)
			continue;
		coord_t other = board_group_other_lib(b, g, to);
		
		/* Let's look at the other liberty of that group. */
		if (immediate_liberty_count(b, other) >= 2 ||      /* Can escape ! */
		    is_bad_nakade(b, color, to, other, s))  /* Should connect instead */
			return true;
	}
	return false;
}

static inline bool
unreachable_lib_from_neighbors(struct board *b, enum stone color, coord_t to, struct selfatari_state *s,
			       coord_t lib)
{
	for (int i = 0; i < s->groupcts[color]; i++) {
		group_t g = s->groupids[color][i];
		for (int j = 0; j < board_group_info(b, g).libs; j++)
			if (board_group_info(b, g).lib[j] == lib)
				return false;
	}
	return true;
}

/* This only looks at existing empty spots, not captures */
static inline bool
capture_would_make_extra_eye(struct board *b, enum stone color, coord_t to, struct selfatari_state *s)
{
	foreach_neighbor(b, to, {
		if (board_at(b, c) == S_NONE)
			if (unreachable_lib_from_neighbors(b, color, to, s, c))
				return true;
	});
	return false;
}

/* Only cares about dead shape. */
static bool
nakade_making_dead_shape(struct board *b, enum stone color, coord_t to, int stones)
{
	assert(stones >= 1);
	assert(stones <= 5);
	
	int dead_shape;
	/* Play self-atari */
	with_move_strict(b, to, color, {	
		/* Play capture */
		group_t g = group_at(b, to);
		with_move_strict(b, board_group_info(b, g).lib[0], stone_other(color), {
				assert(!group_at(b, to));
				dead_shape = nakade_dead_shape(b, to, stone_other(color));
		});
	});
	return dead_shape;	
}

static bool
useful_nakade_making_dead_shape(struct board *b, enum stone color, coord_t to, struct selfatari_state *s,
				bool atariing_group, int stones)
{
	int cap_would_make_eye = false;
	
	assert(stones >= 1);
	assert(stones <= 5);
	
	/* If not atariing surrounding group it's a good move if:
	 *   - Shape after capturing us is dead   AND 
	 *     - Oponent gets extra eye if he plays first OR
	 *     - would create living shape
	 *
	 * If atariing surrounding group we only care about dead shape. */
	
	if (!atariing_group)
		cap_would_make_eye = capture_would_make_extra_eye(b, color, to, s);
	/* TODO: If there's so much eye space that even with filling + capture
	 *       opponent still makes an extra eye it's a silly move. */
     
	/* Can oponent make living shape if we don't play ?
	 * (don't bother killing stuff that's already dead...) */
	if (!atariing_group && !cap_would_make_eye && s->groupcts[color] == 1) 
        {
	    int would_live = false;
	    int prev_neighbor = neighbor_count_at(b, to, color);

	    /* Play opponent color where we want to play */
	    with_move(b, to, stone_other(color), {

		/* Had 2 libs ? One more move to capture then */
		if (prev_neighbor == neighbor_count_at(b, to, color)) {
			group_t standing = -1;	    
			foreach_neighbor(b, to, {
				group_t g = group_at(b, c);		    
				if (board_at(b, c) == color) {
					assert(board_group_info(b, g).libs == 1);  /* Should be in atari */
					standing = g;
				}
			});
			assert(standing != -1);
			
			with_move_strict(b, board_group_info(b, standing).lib[0], stone_other(color), {
				/* Empty now since it's been captured */
				coord_t empty = group_base(s->groupids[color][0]);
				would_live = !nakade_dead_shape(b, empty, stone_other(color));
			});
		}
		else {  /* Empty now since it's been captured */			
			coord_t empty = group_base(s->groupids[color][0]);
			would_live = !nakade_dead_shape(b, empty, stone_other(color));
		}
	    });

	    if (!would_live)	/* And !cap_would_make_eye here */
		    return false;   /* Bad nakade */
	}
	
	return nakade_making_dead_shape(b, color, to, stones);
}


/* More complex throw-in, or in-progress capture from
 * the inside - we are in one of several situations:
 * a O O O O X  b O O O X  c O O O X  d O O O O O
 *   O . X . O    O X . .    O . X .    O . X . O
 *   # # # # #    # # # #    # # # #    # # # # #
 * Throw-ins have been taken care of in check_throwin(),
 * so it's either b or d now:
 * - b is desirable here (since maybe O has no backup two eyes)
 * - d is desirable if putting group in atari (otherwise we
 *   would never capture a single-eyed group). */
#define check_throw_in_or_inside_capture(b, color, to, s, capturing)			\
	if (s->groupcts[color] == 1 && group_is_onestone(b, s->groupids[color][0])) {	\
		group_t g2 = s->groupids[color][0];					\
		assert(board_group_info(b, g2).libs <= 2);				\
		if (board_group_info(b, g2).libs == 1)					\
			return false;  /*  b */						\
		return !capturing;							\
	}

/* Return first empty neighbour found */
static inline coord_t
empty_neighbour(struct board *b, coord_t to)
{
	foreach_neighbor(b, to, {
		if (board_at(b, c) == S_NONE)
			return c;
	});
	assert(0);
}

static inline bool
capturing_from_inside(struct board *b, enum stone color, coord_t to, struct selfatari_state *s,
		      coord_t *plib2)
{
	if (!s->ataris)
		return false;
	
	/* Look at the enemy groups and determine the other contended
	 * liberty. We must make sure the liberty:
	 * (i) is an internal liberty
	 * (ii) filling it to capture our group will not gain safety */
	coord_t lib2 = pass;
	for (int i = 0; i < s->groupcts[stone_other(color)]; i++) {
		group_t g = s->groupids[stone_other(color)][i];
		if (board_group_info(b, g).libs != 2)
			continue;
		
		coord_t this_lib2 = board_group_other_lib(b, g, to);
		if (is_pass(lib2)) 
			*plib2 = lib2 = this_lib2;
		else if (this_lib2 != lib2) 
			/* If we have two neighboring groups that do
			 * not share the other liberty, this for sure
			 * is not inside capture. */
			return false;
	}

	assert (!is_pass(lib2));
	//fprintf(stderr, "  lib2: %s\n", coord2sstr(lib2, b));

	// Throwing single stone in, lib2 must be our lib
	if (!s->groupcts[color])
		return (empty_neighbour(b, to) == lib2);
	
	/* We would create more than 2-stone group; in that
	 * case, the liberty of our result must be lib2,
	 * indicating this really is a nakade capture. */
	for (int j = 0; j < s->groupcts[color]; j++) {
		group_t g2 = s->groupids[color][j];
		assert(board_group_info(b, g2).libs <= 2);
		if (board_group_info(b, g2).lib[0] != lib2 &&
		    (board_group_info(b, g2).libs < 2 || board_group_info(b, g2).lib[1] != lib2))
				return false;
	}

	return true;
}


/* There is another possibility - we can self-atari if it is
 * a nakade: we put an enemy group in atari from the inside.
 * This branch also allows eyes falsification:
 * O O O . .  (This is different from throw-in to false eye
 * X X O O .  checked below in that there is no X stone at the
 * X . X O .  right of the star point in this diagram.)
 * X X X O O
 * X O * . . 
 * We also allow to only nakade if the created shape is dead
 * (http://senseis.xmp.net/?Nakade). */
static int
setup_nakade(struct board *b, enum stone color, coord_t to, struct selfatari_state *s)
{
	// Capturing from inside ?
	coord_t lib2 = pass;
 	bool capturing = capturing_from_inside(b, color, to, s, &lib2);
	
	// If we have other stones nearby capturing us will make 2 eyes
	if (capturing && is_bad_nakade(b, color, to, lib2, s))
		return -1;

	if (!s->groupcts[color])
		return !capturing;  /* Simple throw-in, an easy case */               

	/* Before checking if it's a useful nakade
	 * make sure it can't connect out ! */
	if (can_escape_instead(b, color, to, s))
		return -1;
	
	/* Creating a 2-stone group  ? */
	check_throw_in_or_inside_capture(b, color, to, s, capturing);
		
	int stones = 0;
	for (int j = 0; j < s->groupcts[color]; j++) {
		group_t g2 = s->groupids[color][j];
		stones += group_stone_count(b, g2, 6);
		if (stones > 5)
			return true;
	}
		
	return (useful_nakade_making_dead_shape(b, color, to, s, capturing, stones) ? false : -1);
}

static int
setup_nakade_big_group_only(struct board *b, enum stone color, coord_t to, struct selfatari_state *s)
{
	// Capturing from inside ?
	//coord_t lib2 = pass;
 	//bool capturing = capturing_from_inside(b, color, to, s, &lib2);

	// Throwing a single stone in ? Fine.
	if (!s->groupcts[color])
		return false;
	
	/* Before checking if it's a useful nakade
	 * make sure it can't connect out ! */
	if (can_escape_instead(b, color, to, s))
		return true;
	
	/* Creating a 2-stone group ? Fine too */
	/* Could still be really stupid though
	 * (if can countercapture instead for ex) */
	if (s->groupcts[color] == 1 && group_is_onestone(b, s->groupids[color][0]))
		return false;
	
	/* Making 3-stone group or more */	
	
	/* Not always true but for the most part, if we're creating
	 * a 3+ stone group in atari and we could have captured
	 * something instead it's really stupid, even if shape is
	 * dead locally. */
	for (int j = 0; j < s->groupcts[color]; j++) {
		group_t g2 = s->groupids[color][j];
		if (can_countercapture(b, g2, NULL, 0))
			return true;
	}
	
	int stones = 0;
	for (int j = 0; j < s->groupcts[color]; j++) {
		group_t g2 = s->groupids[color][j];
		stones += group_stone_count(b, g2, 6);
		if (stones > 5)
			return true;
	}
	
	return (nakade_making_dead_shape(b, color, to, stones) ? false : true);
}

#if 0
/* Fast but there are issues with this (triangle six is not dead !)
 * We also need to know status if opponent plays first */
static inline int
nakade_making_dead_shape_hack(struct board *b, enum stone color, coord_t to, int lib2,
			      struct selfatari_state *s, int stones)
{
	/* It also remains to be seen whether it is nakade
	 * and not seki destruction. To do this properly, we
	 * would have to look at the group shape. But we can
	 * cheat too! Brett Combs helps to introduce a static
	 * rule that should in fact cover *all* cases:
	 * 1. Total number of pre-selfatari nakade stones must
	 *    be 5 or smaller. (See above for that.)
	 * 2. If the selfatari is 8-touching all nakade stones,
	 *    it is proper nakade.
	 * 3. Otherwise, there must be only a single nakade
	 *    group, it must be at least 4-stone and its other
	 *    liberty must be 8-touching the same number of
	 *    stones as us. */
	int touch8 = neighbor_count_at(b, to, color);
	foreach_diag_neighbor(b, to) {
		if (board_at(b, c) != color) continue;
		/* Consider only internal stones. Otherwise, e.g.
		 * X O . X
		 * X . O X  can make trouble, bottom O is
		 * O X X X  irrelevant. */
		if (board_group_info(b, group_at(b, c)).lib[0] == to
		    || board_group_info(b, group_at(b, c)).lib[1] == to)
			touch8++;
	} foreach_diag_neighbor_end;
	if (touch8 == stones)
		return true;

	if (s->groupcts[color] > 1)
		return false;
	if (stones == 3)   // 4 stones and self-atari not 8-connected to all of them -> living shape
		return false;
	if (stones < 3)    // always dead shape
		return true;
	
	int ltouch8 = neighbor_count_at(b, lib2, color);
	foreach_diag_neighbor(b, lib2) {
		if (board_at(b, c) != color) continue;
		if (board_group_info(b, group_at(b, c)).lib[0] == to
		    || board_group_info(b, group_at(b, c)).lib[1] == to)
			ltouch8++;
	} foreach_diag_neighbor_end;
	return ltouch8 == touch8;
}
#endif

/*  Throwing one stone to make a ko:
 *  . . O O O . O * . X 
 *  O X O O X O O O X . 
 *  O O X X X X X O O X 
 *  . . . . . . X X X X
 */
static bool
simple_throwin_making_ko(struct board *b, enum stone color, coord_t to, struct selfatari_state *s)
{
        // Find our liberty ...
        foreach_neighbor(b, to, {
                        if (board_at(b, c) == S_NONE)
                                return is_controlled_eye_point(b, c, color);
                });
        assert(0);
}


static int
check_throwin(struct board *b, enum stone color, coord_t to, struct selfatari_state *s)
{
        if (s->groupcts[color] == 0 &&
	    simple_throwin_making_ko(b, color, to, s))
		return false;

	/* We can be throwing-in to false eye:
	 * X X X O X X X O X X X X X
	 * X . * X * O . X * O O . X
	 * # # # # # # # # # # # # # */
	/* We cannot sensibly throw-in into a corner. */
	if (neighbor_count_at(b, to, S_OFFBOARD) < 2
	    && neighbor_count_at(b, to, stone_other(color))
	       + neighbor_count_at(b, to, S_OFFBOARD) == 3
	    && board_is_false_eyelike(b, to, stone_other(color))) {
		assert(s->groupcts[color] <= 1);
		/* Single-stone throw-in may be ok... */
		if (s->groupcts[color] == 0) {
			/* O X .  There is one problem - when it's
			 * . * X  actually not a throw-in!
			 * # # #  */
			foreach_neighbor(b, to, {
				if (board_at(b, c) == S_NONE) {
					/* Is the empty neighbor an escape path? */
					/* (Note that one S_NONE neighbor is already @to.) */
					if (neighbor_count_at(b, c, stone_other(color))
					    + neighbor_count_at(b, c, S_OFFBOARD) < 2)
						return -1;
				}
			});
			return false;
		}

		/* Multi-stone throwin...? */
		assert(s->groupcts[color] == 1);
		group_t g = s->groupids[color][0];

		assert(board_group_info(b, g).libs <= 2);
		/* Suicide is definitely NOT ok, no matter what else
		 * we could test. */
		if (board_group_info(b, g).libs == 1)
			return true;

		/* In that case, we must be connected to at most one stone,
		 * or throwin will not destroy any eyes. */
		if (group_is_onestone(b, g))
			return false;
	}
	return -1;
}

static void
init_selfatari_state(struct board *b, enum stone color, coord_t to, struct selfatari_state *s)
{
	memset(s, 0, sizeof(*s));

	foreach_neighbor(b, to, {
		enum stone color = board_at(b, c);
		group_t group = group_at(b, c);
		bool dup = false;
		for (int i = 0; i < s->groupcts[color]; i++)
			if (s->groupids[color][i] == group) {
				dup = true;
				break;
			}
		if (!dup) {
			s->groupneis[color][s->groupcts[color]] = c;
			s->groupids[color][s->groupcts[color]++] = group_at(b, c);
		}
	});	
}

bool
is_bad_selfatari_slow(struct board *b, enum stone color, coord_t to, int flags)
{
	if (DEBUGL(5))
		fprintf(stderr, "sar check %s %s\n", stone2str(color), coord2sstr(to, b));
	/* Assess if we actually gain any liberties by this escape route.
	 * Note that this is not 100% as we cannot check whether we are
	 * connecting out or just to ourselves. */

	struct selfatari_state s;
	init_selfatari_state(b, color, to, &s);
	
	/* We have shortage of liberties; that's the point. */
	assert(s.groupcts[S_NONE] <= 1);

	int d;
	d = examine_friendly_groups(b, color, to, &s, flags);
	if (d >= 0)     return d;
	if (DEBUGL(6))  fprintf(stderr, "no friendly group\n");
	
	d = examine_enemy_groups(b, color, to, &s);
	if (d >= 0)	return d;
	if (DEBUGL(6))  fprintf(stderr, "no capture\n");
	
	if (!(flags & SELFATARI_BIG_GROUPS_ONLY)) {
		d = check_throwin(b, color, to, &s);
		if (d >= 0)	return d;
		if (DEBUGL(6))	fprintf(stderr, "no throw-in group\n");
	}	

	if (flags & SELFATARI_BIG_GROUPS_ONLY)
		d = setup_nakade_big_group_only(b, color, to, &s);
	else
		d = setup_nakade(b, color, to, &s);
	if (d >= 0)	return d;
	if (DEBUGL(6))	fprintf(stderr, "no nakade group\n");	

	/* No way to pull out, no way to connect out. This really
	 * is a bad self-atari! */
	return true;
}


coord_t
selfatari_cousin(struct board *b, enum stone color, coord_t coord, group_t *bygroup)
{
	group_t groups[4]; int groups_n = 0;
	int groupsbycolor[4] = {0, 0, 0, 0};
	if (DEBUGL(6))
		fprintf(stderr, "cousin group search: ");
	foreach_neighbor(b, coord, {
		enum stone s = board_at(b, c);
		group_t g = group_at(b, c);
		if (board_group_info(b, g).libs == 2) {
			groups[groups_n++] = g;
			groupsbycolor[s]++;
			if (DEBUGL(6))
				fprintf(stderr, "%s(%s) ", coord2sstr(c, b), stone2str(s));
		}
	});
	if (DEBUGL(6))
		fprintf(stderr, "\n");

	if (!groups_n)
		return pass;

	int gn;
	if (groupsbycolor[stone_other(color)]) {
		/* Prefer to fill the other liberty of an opponent
		 * group to filling own approach liberties. */
		int gl = fast_random(groups_n);
		for (gn = gl; gn < groups_n; gn++)
			if (board_at(b, groups[gn]) == stone_other(color))
				goto found;
		for (gn = 0; gn < gl; gn++)
			if (board_at(b, groups[gn]) == stone_other(color))
				goto found;
found:;
	} else {
		gn = fast_random(groups_n);
	}
	int gl = gn;
	for (; gn - gl < groups_n; gn++) {
		int gnm = gn % groups_n;
		group_t group = groups[gnm];

		coord_t lib2;
		/* Can we get liberties by capturing a neighbor? */
		struct move_queue ccq; ccq.moves = 0;
		if (board_at(b, group) == color &&
		    can_countercapture(b, group, &ccq, 0)) {
			lib2 = mq_pick(&ccq);

		} else {
			lib2 = board_group_other_lib(b, group, coord);
			if (board_is_one_point_eye(b, lib2, board_at(b, group)))
				continue;
			if (is_bad_selfatari(b, color, lib2))
				continue;
		}
		if (bygroup)
			*bygroup = group;
		return lib2;
	}
	return pass;
}
