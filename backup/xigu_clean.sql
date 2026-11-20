-- XiguChat 数据库快照（干净库：仅账号、好友关系与一个空会话）
-- 适用：MySQL 8.0+ / 9.x，字符集 utf8mb4
-- 导入：mysql -uroot -p xigu < backup/xigu.sql
--
-- 表关系：
--   user                账号与公开资料
--   friend              好友关系，双向各一行；主键即 (self_id, friend_id)
--   friend_apply        好友申请，一对 (from_uid,to_uid) 只有一条，重新申请复位状态
--   conversation        单聊与群聊的统一会话；单聊用 dm_key 去重，群聊用 state 软删
--   conversation_member 成员、角色、群昵称、免打扰与已读游标
--   chat_message        消息主表；幂等键 (thread_id,sender_id,client_msg_id)
--   chat_message_archive 归档表，无外键，按 created_at 保留一年

SET NAMES utf8mb4;
SET TIME_ZONE = '+00:00';
SET FOREIGN_KEY_CHECKS = 0;
SET UNIQUE_CHECKS = 0;

DROP TABLE IF EXISTS `chat_message`;
DROP TABLE IF EXISTS `chat_message_archive`;
DROP TABLE IF EXISTS `conversation_member`;
DROP TABLE IF EXISTS `conversation`;
DROP TABLE IF EXISTS `friend`;
DROP TABLE IF EXISTS `friend_apply`;
DROP TABLE IF EXISTS `user`;

-- ---------------------------------------------------------------------------
-- 账号
-- name/email 唯一；pwd 存 libsodium Argon2id 字符串，不存明文
-- ---------------------------------------------------------------------------
CREATE TABLE `user` (
  `uid`   int NOT NULL AUTO_INCREMENT,
  `name`  varchar(64)  COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `email` varchar(254) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `pwd`   varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `nick`  varchar(64)  COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `desc`  varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `sex`   tinyint unsigned NOT NULL DEFAULT 0 COMMENT '0 未知 1 男 2 女',
  `icon`  varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  PRIMARY KEY (`uid`),
  UNIQUE KEY `uk_user_name` (`name`),
  UNIQUE KEY `uk_user_email` (`email`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- 好友关系：双向各一行，备注与标签跟随各自方向
-- ---------------------------------------------------------------------------
CREATE TABLE `friend` (
  `self_id`   int NOT NULL,
  `friend_id` int NOT NULL,
  `back`      varchar(64)  COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `tags`      varchar(512) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '[]',
  PRIMARY KEY (`self_id`,`friend_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- 好友申请：重新申请时复用同一行，只把状态复位为 0
-- ---------------------------------------------------------------------------
CREATE TABLE `friend_apply` (
  `id`        bigint unsigned NOT NULL AUTO_INCREMENT,
  `from_uid`  int NOT NULL,
  `to_uid`    int NOT NULL,
  `status`    tinyint unsigned NOT NULL DEFAULT 0 COMMENT '0 待处理 1 已通过 2 已拒绝',
  `descs`     varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `back_name` varchar(64)  COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `tags`      varchar(512) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '[]',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_apply_pair` (`from_uid`,`to_uid`),
  KEY `idx_apply_to` (`to_uid`,`id`),
  KEY `idx_apply_from` (`from_uid`,`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- 会话：单聊与群聊共用；thread_type 1 单聊 / 2 群聊
-- state=1 表示群已解散，成员与消息保留，列表查询按 state=0 过滤
-- ---------------------------------------------------------------------------
CREATE TABLE `conversation` (
  `thread_id`       bigint unsigned NOT NULL AUTO_INCREMENT,
  `thread_type`     tinyint unsigned NOT NULL DEFAULT 1,
  `dm_key`          varchar(32) COLLATE utf8mb4_unicode_ci DEFAULT NULL COMMENT '单聊去重键：小uid:大uid',
  `name`            varchar(64) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `owner_id`        int NOT NULL DEFAULT 0,
  `state`           tinyint unsigned NOT NULL DEFAULT 0 COMMENT '0 正常 1 已解散',
  `dissolved_at`    datetime DEFAULT NULL,
  `notice`          varchar(500) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `last_message_id` bigint unsigned NOT NULL DEFAULT 0,
  `last_message_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `created_at`      timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`thread_id`),
  UNIQUE KEY `uk_conversation_dm` (`dm_key`),
  KEY `idx_conversation_last` (`state`,`last_message_at`,`thread_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- 会话成员：last_read_message_id 是唯一已读游标，未读数由它推导
-- ---------------------------------------------------------------------------
CREATE TABLE `conversation_member` (
  `thread_id`            bigint unsigned NOT NULL,
  `uid`                  int NOT NULL,
  `role`                 tinyint unsigned NOT NULL DEFAULT 0 COMMENT '0 成员 1 管理员 2 群主',
  `nickname`             varchar(32) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '' COMMENT '我在本群的昵称',
  `muted`                tinyint unsigned NOT NULL DEFAULT 0,
  `last_read_message_id` bigint unsigned NOT NULL DEFAULT 0,
  `joined_at`            timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`thread_id`,`uid`),
  KEY `idx_member_uid` (`uid`,`thread_id`),
  CONSTRAINT `fk_member_conversation` FOREIGN KEY (`thread_id`)
    REFERENCES `conversation` (`thread_id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- 消息：created_at 与 message_id 同时提供时间显示与会话内排序
-- mentions 是逗号分隔的 uid 串，2KB 请求上限内最多可容纳整群 200 人
-- ---------------------------------------------------------------------------
CREATE TABLE `chat_message` (
  `message_id`   bigint unsigned NOT NULL AUTO_INCREMENT,
  `thread_id`    bigint unsigned NOT NULL,
  `sender_id`    int NOT NULL,
  `recv_id`      int NOT NULL DEFAULT 0 COMMENT '单聊为接收者；群聊固定 0',
  `client_msg_id` varchar(64) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `content`      text COLLATE utf8mb4_unicode_ci NOT NULL,
  `created_at`   timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `state`        tinyint unsigned NOT NULL DEFAULT 0 COMMENT '0 正常 1 已撤回',
  `upload_state` tinyint unsigned NOT NULL DEFAULT 1 COMMENT '0 资源未上传 1 已就绪',
  `msg_type`     tinyint unsigned NOT NULL DEFAULT 0 COMMENT '0 文本 1 图片 2 视频 3 文件 4 群系统消息',
  `mentions`     varchar(2048) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  PRIMARY KEY (`message_id`),
  UNIQUE KEY `uk_chat_client_msg` (`thread_id`,`sender_id`,`client_msg_id`),
  KEY `idx_chat_message_thread` (`thread_id`,`message_id`),
  KEY `idx_chat_created` (`created_at`,`message_id`),
  CONSTRAINT `fk_message_conversation` FOREIGN KEY (`thread_id`)
    REFERENCES `conversation` (`thread_id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- 归档：无外键；归档线程先 INSERT IGNORE 再 DELETE，重复执行安全
-- ---------------------------------------------------------------------------
CREATE TABLE `chat_message_archive` (
  `message_id`   bigint unsigned NOT NULL,
  `thread_id`    bigint unsigned NOT NULL,
  `sender_id`    int NOT NULL,
  `recv_id`      int NOT NULL DEFAULT 0,
  `client_msg_id` varchar(64) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `content`      text COLLATE utf8mb4_unicode_ci NOT NULL,
  `created_at`   timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `state`        tinyint unsigned NOT NULL DEFAULT 0,
  `upload_state` tinyint unsigned NOT NULL DEFAULT 1,
  `msg_type`     tinyint unsigned NOT NULL DEFAULT 0,
  `mentions`     varchar(2048) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `archived_at`  timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`message_id`),
  KEY `idx_archive_thread` (`thread_id`,`message_id`),
  KEY `idx_archive_time` (`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- 演示账号（密码均为 123456，客户端 xor 后发送 745230）
-- ---------------------------------------------------------------------------
INSERT INTO `user` (`uid`,`name`,`email`,`pwd`,`nick`,`desc`,`sex`,`icon`) VALUES
(1001,'西故','3099154616@qq.com','$argon2id$v=19$m=262144,t=3,p=1$bD899q54K8vMwy+5fc7JKA$/jVJP8RyQj0mFzJqY/56T2F9f3SB74NSBm9Lxc0XoZ8','xigu','',0,''),
(1002,'霄宫','xigu920@163.com','$argon2id$v=19$m=262144,t=3,p=1$o/CzQVrUYHFP1kQbYhSztw$7KdjCxJlu0I7uDrmqqIivmuBUfII15hYy66h3Q6H870','烟花','',1,''),
(1003,'test1','123@123.com','$argon2id$v=19$m=65536,t=2,p=1$tXdYqduAQZKxtsyo3ac+rQ$voCSwFb/G2QfrcmexfKS8KLxyTaDS3TWC3bmVmzy6WU','','',0,''),
(1004,'test2','456@456.com','$argon2id$v=19$m=65536,t=2,p=1$gNlpRDd7H5TFIwsvI/uZVQ$sh+mt+qXh0xQ0fA5Ij1E75KRnWrUVl4MMlFihDbccUs','','',0,''),
(1005,'test3','789@789.com','$argon2id$v=19$m=65536,t=2,p=1$8B35hu43rJDUBr0yoqwExA$5HzrZBveaYnAXzXgcKc1d+lixv6vkbvVcWmxOi2F65Q','','',0,'');

INSERT INTO `friend` (`self_id`,`friend_id`,`back`,`tags`) VALUES
(1003,1004,'','["朋友"]'),
(1004,1003,'','[]');

INSERT INTO `conversation` (`thread_id`,`thread_type`,`dm_key`,`name`,`owner_id`,`state`,`dissolved_at`,`notice`,`last_message_id`,`last_message_at`,`created_at`) VALUES
(1,1,'1003:1004','',0,0,NULL,'',0,'2026-11-16 13:00:58','2026-11-16 13:00:58');

INSERT INTO `conversation_member` (`thread_id`,`uid`,`role`,`nickname`,`muted`,`last_read_message_id`,`joined_at`) VALUES
(1,1003,0,'',0,0,'2026-11-16 13:00:58'),
(1,1004,0,'',0,0,'2026-11-16 13:00:58');

SET FOREIGN_KEY_CHECKS = 1;
SET UNIQUE_CHECKS = 1;
