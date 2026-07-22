-- Register area 38 so it is a valid teleport destination.
-- The server UPDATEs this row but never INSERTs it, so it must exist.
INSERT INTO area (ID,mirror,name,players,alive_time,idle,server,port,bps,mem_usage,last_error)
  VALUES (38,1,'Test Map',0,UNIX_TIMESTAMP(),0,0,5593,0,0,0);
