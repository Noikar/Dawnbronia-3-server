--
-- Idempotent schema migrations.
--
-- Unlike create_tables.sql (which only runs against a brand new, empty
-- database), this file is applied on EVERY server boot - both to fresh installs
-- and to databases that were created by an older build. Every statement here
-- must therefore be safe to run repeatedly: use IF NOT EXISTS, guard ALTERs,
-- and never drop or rewrite existing player data.
--

--
-- Archive of characters removed via the in-client "Delete character" option.
-- The column list must stay identical to 'chars' (same order) plus the trailing
-- deleted_time, because db_acc_delete archives a row with
-- "insert chars_deleted select *,<time> from chars where ID=...".
-- Unlike 'chars' the name is not unique here: a name can be created, deleted,
-- recreated and deleted again.
--

CREATE TABLE IF NOT EXISTS chars_deleted (
  ID int(11) NOT NULL default '0',
  name varchar(40) NOT NULL default '',
  class int(11) default NULL,
  karma int(11) default NULL,
  clan int(11) default NULL,
  clan_rank int(11) default NULL,
  clan_serial int(11) default NULL,
  experience int(11) default NULL,
  current_area int(11) default NULL,
  allowed_area int(11) default NULL,
  creation_time int(11) default NULL,
  login_time int(11) default NULL,
  logout_time int(11) default NULL,
  locked enum('Y','N') default NULL,
  sID int(11) default NULL,
  chr blob,
  item blob,
  ppd blob,
  mirror int(11) default NULL,
  current_mirror int(11) default NULL,
  spacer bigint(20) default NULL,
  deleted_time int(11) NOT NULL default '0',
  PRIMARY KEY  (ID),
  KEY name (name),
  KEY sID (sID),
  KEY deleted_time (deleted_time)
);
