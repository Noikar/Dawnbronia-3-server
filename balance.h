/*

$Id: balance.h,v 1.1 2005/09/24 09:55:48 ssim Exp $

$Log: balance.h,v $
Revision 1.1  2005/09/24 09:55:48  ssim
Initial revision

Revision 1.2  2003/10/13 14:11:48  sam
Added RCS tags


*/

#define ATTACK_DIV 5 // orig 5

#define FIREBALL_DAMAGE 5 // orig 5
#define STRIKE_DAMAGE 5 // orig 5

// experience rate multipliers, in percent (100 = original rate)
#define KILL_EXP_RATE 170 // per-kill exp, including first-kill bonuses
#define QUEST_EXP_RATE 145 // quest completion exp, including driver-awarded quest exp
#define PENT_EXP_RATE 150 // pentagram solve exp (main solve reward + clan-jewel reflection)
#define PENT_CLICK_EXP_MULT 120 // per-click pentagram worth multiplier (the "%d exp" shown per pent)
