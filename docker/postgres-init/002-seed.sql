INSERT INTO users (id,email,password_hash,display_name) OVERRIDING SYSTEM VALUE VALUES
 (1,'demo@example.local','$2b$12$development-only-placeholder','Demo User') ON CONFLICT (id) DO NOTHING;
INSERT INTO roles (id,code,name) OVERRIDING SYSTEM VALUE VALUES
 (1,'USER','Project user'),(2,'ADMIN','Administrator'),(3,'OPERATOR','Operator') ON CONFLICT (id) DO NOTHING;
INSERT INTO user_roles (user_id,role_id) VALUES (1,1) ON CONFLICT DO NOTHING;
INSERT INTO projects (id,name,status,created_by) OVERRIDING SYSTEM VALUE VALUES (1,'Demo project','ACTIVE',1) ON CONFLICT (id) DO NOTHING;
INSERT INTO project_members (project_id,user_id,role_code) VALUES (1,1,'OWNER') ON CONFLICT DO NOTHING;
INSERT INTO quotas (project_id,resource,limit_value) VALUES (1,'VCPU',16),(1,'MEMORY_MB',32768),(1,'DISK_GB',500),(1,'INSTANCE',10) ON CONFLICT (project_id,resource) DO NOTHING;
INSERT INTO images (id,provider_image_id,name,os_family,version) OVERRIDING SYSTEM VALUE VALUES (1,'demo-ubuntu-2204','Ubuntu 22.04','Ubuntu','22.04') ON CONFLICT (id) DO NOTHING;
INSERT INTO flavors (id,name,vcpu,memory_mb,disk_gb) OVERRIDING SYSTEM VALUE VALUES
 (1,'small',2,4096,40),
 (2,'medium',4,8192,80),
 (3,'large',6,12288,120),
 (4,'compute',8,16384,160),
 (5,'memory',8,32768,200)
ON CONFLICT (id) DO NOTHING;
SELECT setval(pg_get_serial_sequence('users','id'),GREATEST((SELECT max(id) FROM users),1));
SELECT setval(pg_get_serial_sequence('roles','id'),GREATEST((SELECT max(id) FROM roles),1));
SELECT setval(pg_get_serial_sequence('projects','id'),GREATEST((SELECT max(id) FROM projects),1));
SELECT setval(pg_get_serial_sequence('images','id'),GREATEST((SELECT max(id) FROM images),1));
SELECT setval(pg_get_serial_sequence('flavors','id'),GREATEST((SELECT max(id) FROM flavors),1));

