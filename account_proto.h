/*
 * Part of Astonia Server. Please read license.txt.
 *
 * Account sub-protocol (pre-login registration and character management).
 *
 * This runs on the normal game port, before login, over a short-lived
 * connection per operation. The server tells an account request apart from a
 * classic login by the FIRST byte: a login always begins with an alphabetic
 * character-name byte, so the op codes below are deliberately non-alpha.
 *
 * IMPORTANT: keep this file byte-for-byte identical to the copy in the client
 * repo (astonia_community_client/src/client/account_proto.h).
 */

#ifndef ACCOUNT_PROTO_H
#define ACCOUNT_PROTO_H

// Field widths on the wire. These match the login blob: ACC_NAMELEN ==
// sizeof(ch[].name) (40) and ACC_PWLEN == MAXPASSWORD (16).
#define ACC_NAMELEN 40
#define ACC_PWLEN 16

// Request op codes (first byte of the request). Non-alpha on purpose.
#define ACC_OP_REGISTER 0x01
#define ACC_OP_LIST 0x02
#define ACC_OP_CREATE 0x03
#define ACC_OP_DELETE 0x04 // reserved for a later phase

// CREATE character flags byte.
#define ACC_FLAG_MALE 0x01 // clear = female
#define ACC_FLAG_WARRIOR 0x02 // warrior bit
#define ACC_FLAG_MAGE 0x04 // mage bit (warrior+mage both set = seyan)
#define ACC_FLAG_ARCH 0x08 // arch variant - honored only for admin accounts
#define ACC_FLAG_GOD 0x10 // god powers - honored only for admin accounts

// Per-account character cap (also the max entries in a LIST reply).
#define ACC_MAXCHARS 8

// Request layouts (all fixed size, little-endian x86 both ends):
//   REGISTER / LIST : op(1) + username(ACC_NAMELEN) + pw(ACC_PWLEN)
//   CREATE          : op(1) + username(ACC_NAMELEN) + pw(ACC_PWLEN)
//                     + charname(ACC_NAMELEN) + flags(1)
// The password field is obfuscated with the same XOR scheme login uses, keyed
// by the account username.
#define ACC_REQ_BASE (1 + ACC_NAMELEN + ACC_PWLEN) // REGISTER / LIST
#define ACC_REQ_CREATE (ACC_REQ_BASE + ACC_NAMELEN + 1) // CREATE

// Reply. The server writes this RAW (uncompressed) with a direct csend, so it
// arrives right after the connection's tiny uncompressed SV_REALTIME greeting
// frame (which the client skips by its length header). Layout:
//   u8  magic     = ACC_REPLY_MAGIC
//   u8  op        = echoed op
//   u8  status    = ACC_ST_*
//   u8  count     = number of list entries (LIST only, else 0)
//   u8  acctflags = account-level flags (ACC_ACCT_*); 0 unless authenticated
//   count x entry:
//     u8  namelen
//     u8  name[namelen]
//     u32 flags  (character class flag bits)
//     u32 exp    (experience)
#define ACC_REPLY_MAGIC 0xA5

// Account-level reply flags (the acctflags byte).
#define ACC_ACCT_ADMIN 0x01 // account may create arch / god characters

#define ACC_ST_OK 0
#define ACC_ST_BADCREDS 1
#define ACC_ST_TAKEN 2
#define ACC_ST_INVALID 3
#define ACC_ST_LIMIT 4
#define ACC_ST_SERVERERR 5

#endif
