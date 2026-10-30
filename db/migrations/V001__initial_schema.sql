-- VMaaS initial relational model. PostgreSQL 15+
CREATE TABLE users (
    id              BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    email           VARCHAR(254) NOT NULL UNIQUE,
    password_hash   VARCHAR(255) NOT NULL,
    display_name    VARCHAR(120) NOT NULL,
    status          VARCHAR(16) NOT NULL DEFAULT 'ACTIVE' CHECK (status IN ('ACTIVE','DISABLED')),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE roles (
    id          SMALLINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    code        VARCHAR(32) NOT NULL UNIQUE,
    name        VARCHAR(80) NOT NULL,
    CHECK (code IN ('USER','ADMIN','OPERATOR'))
);

CREATE TABLE user_roles (
    user_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role_id     SMALLINT NOT NULL REFERENCES roles(id) ON DELETE RESTRICT,
    granted_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, role_id)
);

CREATE TABLE projects (
    id          BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name        VARCHAR(120) NOT NULL,
    status      VARCHAR(16) NOT NULL DEFAULT 'ACTIVE' CHECK (status IN ('ACTIVE','SUSPENDED','ARCHIVED')),
    created_by  BIGINT REFERENCES users(id) ON DELETE SET NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (name)
);

CREATE TABLE project_members (
    project_id  BIGINT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    user_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role_code   VARCHAR(16) NOT NULL CHECK (role_code IN ('OWNER','MEMBER','VIEWER')),
    joined_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (project_id, user_id)
);

CREATE TABLE quotas (
    id           BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    project_id   BIGINT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    resource     VARCHAR(16) NOT NULL CHECK (resource IN ('VCPU','MEMORY_MB','DISK_GB','INSTANCE')),
    limit_value  BIGINT NOT NULL CHECK (limit_value >= 0),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (project_id, resource)
);

CREATE TABLE images (
    id                 BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    provider_image_id  VARCHAR(128) NOT NULL UNIQUE,
    name               VARCHAR(120) NOT NULL,
    os_family          VARCHAR(48) NOT NULL,
    version            VARCHAR(48) NOT NULL,
    active             BOOLEAN NOT NULL DEFAULT TRUE,
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE flavors (
    id          BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name        VARCHAR(80) NOT NULL UNIQUE,
    vcpu        INTEGER NOT NULL CHECK (vcpu > 0),
    memory_mb   INTEGER NOT NULL CHECK (memory_mb > 0),
    disk_gb     INTEGER NOT NULL CHECK (disk_gb > 0),
    active      BOOLEAN NOT NULL DEFAULT TRUE
);

CREATE TABLE orders (
    id                 UUID PRIMARY KEY,
    project_id         BIGINT NOT NULL REFERENCES projects(id) ON DELETE RESTRICT,
    requested_by       BIGINT NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    image_id           BIGINT NOT NULL REFERENCES images(id) ON DELETE RESTRICT,
    flavor_id          BIGINT NOT NULL REFERENCES flavors(id) ON DELETE RESTRICT,
    vm_name            VARCHAR(63) NOT NULL,
    state              VARCHAR(20) NOT NULL CHECK (state IN ('PENDING','PROVISIONING','READY','FAILED','DELETING','DELETED','CANCELLED')),
    idempotency_key    VARCHAR(128) NOT NULL,
    correlation_id     UUID NOT NULL,
    failure_code       VARCHAR(64),
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (project_id, requested_by, idempotency_key)
);

CREATE TABLE virtual_machines (
    id                 BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    order_id           UUID NOT NULL UNIQUE REFERENCES orders(id) ON DELETE RESTRICT,
    project_id         BIGINT NOT NULL REFERENCES projects(id) ON DELETE RESTRICT,
    provider_vm_id     VARCHAR(128) UNIQUE,
    name               VARCHAR(63) NOT NULL,
    state              VARCHAR(20) NOT NULL CHECK (state IN ('BUILD','ACTIVE','STOPPED','ERROR','DELETING','DELETED')),
    private_ip         INET,
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    deleted_at         TIMESTAMPTZ
);

CREATE TABLE order_events (
    id           BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    order_id     UUID NOT NULL REFERENCES orders(id) ON DELETE CASCADE,
    from_state   VARCHAR(20),
    to_state     VARCHAR(20) NOT NULL,
    event_type   VARCHAR(64) NOT NULL,
    details      JSONB NOT NULL DEFAULT '{}'::jsonb,
    occurred_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE resource_reservations (
    id           BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    order_id     UUID NOT NULL REFERENCES orders(id) ON DELETE RESTRICT,
    project_id   BIGINT NOT NULL REFERENCES projects(id) ON DELETE RESTRICT,
    resource     VARCHAR(16) NOT NULL CHECK (resource IN ('VCPU','MEMORY_MB','DISK_GB','INSTANCE')),
    amount       BIGINT NOT NULL CHECK (amount > 0),
    status       VARCHAR(12) NOT NULL DEFAULT 'HELD' CHECK (status IN ('HELD','COMMITTED','RELEASED')),
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    released_at  TIMESTAMPTZ,
    UNIQUE (order_id, resource)
);

CREATE TABLE refresh_tokens (
    id          BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token_hash  VARCHAR(255) NOT NULL UNIQUE,
    expires_at  TIMESTAMPTZ NOT NULL,
    revoked_at  TIMESTAMPTZ,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE audit_log (
    id              BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    actor_user_id   BIGINT REFERENCES users(id) ON DELETE SET NULL,
    project_id      BIGINT REFERENCES projects(id) ON DELETE SET NULL,
    action          VARCHAR(80) NOT NULL,
    object_type     VARCHAR(48) NOT NULL,
    object_id       VARCHAR(128),
    correlation_id  UUID,
    details         JSONB NOT NULL DEFAULT '{}'::jsonb,
    occurred_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE outbox_events (
    id              UUID PRIMARY KEY,
    aggregate_type  VARCHAR(48) NOT NULL,
    aggregate_id    VARCHAR(128) NOT NULL,
    event_type      VARCHAR(80) NOT NULL,
    schema_version  SMALLINT NOT NULL DEFAULT 1,
    payload         JSONB NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    published_at    TIMESTAMPTZ,
    attempts        INTEGER NOT NULL DEFAULT 0 CHECK (attempts >= 0)
);

CREATE TABLE metric_samples (
    id           BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    vm_id        BIGINT NOT NULL REFERENCES virtual_machines(id) ON DELETE CASCADE,
    metric_name  VARCHAR(80) NOT NULL,
    value        DOUBLE PRECISION NOT NULL,
    unit         VARCHAR(24) NOT NULL,
    observed_at  TIMESTAMPTZ NOT NULL
);

CREATE INDEX idx_orders_project_state_created ON orders(project_id, state, created_at DESC);
CREATE INDEX idx_vms_project_state ON virtual_machines(project_id, state);
CREATE INDEX idx_order_events_order_time ON order_events(order_id, occurred_at);
CREATE INDEX idx_reservations_project_status ON resource_reservations(project_id, status);
CREATE INDEX idx_outbox_unpublished ON outbox_events(created_at) WHERE published_at IS NULL;
CREATE INDEX idx_metrics_vm_name_time ON metric_samples(vm_id, metric_name, observed_at DESC);
