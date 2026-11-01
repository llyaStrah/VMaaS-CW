
const $ = (selector) => document.querySelector(selector);

const state = {
    projects: [],
    flavors: [],
    images: [],
    orders: []
};

let refreshTimer;

const labels = {
    PENDING: 'Ожидает',
    PROVISIONING: 'Развертывается',
    READY: 'Готова',
    FAILED: 'Ошибка',
    DELETING: 'Удаляется',
    DELETED: 'Удалена',
    CANCELLED: 'Отменена'
};

async function api(path, options = {}) {
    const token = localStorage.getItem('vmaasToken');

    const response = await fetch(path, {
        ...options,
        headers: {
            ...(options.body
                ? { 'Content-Type': 'application/json' }
                : {}),
            ...(token &&
                !path.includes('/auth/login') &&
                !path.includes('/auth/register')
                ? { Authorization: `Bearer ${token}` }
                : {}),
            ...options.headers
        }
    });

    const data = await response.json().catch(() => ({}));

    if (
        response.status === 401 &&
        !path.includes('/auth/login') &&
        !path.includes('/auth/register')
    ) {
        localStorage.removeItem('vmaasToken');
        $('#auth-screen').hidden = false;
    }

    if (!response.ok) {
        throw new Error(data.message || `Ошибка запроса (${response.status})`);
    }

    return data;
}

function esc(value) {
    return String(value ?? '').replace(/[&<>"']/g, (c) => ({
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        '"': '&quot;',
        "'": '&#39;'
    })[c]);
}

function stateClass(s) {
    return String(s || '').toLowerCase();
}

function formatDate(value) {
    return value
        ? new Date(value).toLocaleString('ru-RU', {
            day: '2-digit',
            month: 'short',
            hour: '2-digit',
            minute: '2-digit'
        })
        : '—';
}

async function loadCatalog() {
    const [projects, flavors, images] = await Promise.all([
        api('/api/v1/projects'),
        api('/api/v1/catalog/flavors'),
        api('/api/v1/catalog/images')
    ]);

    state.projects = projects.items || [];
    state.flavors = flavors.items || [];
    state.images = images.items || [];

    $('#project').innerHTML = state.projects
        .map((p) =>
            `<option value="${esc(p.id)}">${esc(p.name)} · ${esc(p.role)}</option>`
        )
        .join('');

    $('#flavor').innerHTML = state.flavors
        .map((f) =>
            `<option value="${esc(f.id)}">${esc(f.name)} — ${f.vcpu} vCPU · ${Math.round(f.memoryMb / 1024)} ГБ RAM · ${f.diskGb} ГБ</option>`
        )
        .join('');

    $('#image').innerHTML = state.images
        .map((i) =>
            `<option value="${esc(i.id)}">${esc(i.name)} · ${esc(i.osFamily)} ${esc(i.version)}</option>`
        )
        .join('');

    $('#stat-flavors').textContent = state.flavors.length;

    renderFlavor();
}

function renderFlavor() {
    const f = state.flavors.find(
        (x) => String(x.id) === $('#flavor').value
    );

    $('#flavor-preview').innerHTML = f
        ? `
            <span class="spec-chip">
                CPU <b>${f.vcpu} vCPU</b>
            </span>
            <span class="spec-chip">
                Память <b>${Math.round(f.memoryMb / 1024)} ГБ RAM</b>
            </span>
            <span class="spec-chip">
                Диск <b>${f.diskGb} ГБ SSD</b>
            </span>
        `
        : '';
}

async function loadOrders() {
    try {
        const result = await api('/api/v1/orders');

        state.orders = result.items || [];

        $('#stat-total').textContent = state.orders.length;

        $('#stat-ready').textContent = state.orders.filter(
            (o) => o.state === 'READY'
        ).length;

        $('#stat-pending').textContent = state.orders.filter(
            (o) => ['PENDING', 'PROVISIONING'].includes(o.state)
        ).length;

        $('#orders-body').innerHTML = state.orders.length
            ? state.orders
                .map((o) => `
                    <tr>
                        <td>
                            <span class="vm-name">${esc(o.vmName)}</span>
                            <span class="vm-sub">${esc(o.orderId)}</span>
                        </td>
                        <td>${esc(o.flavor)}</td>
                        <td>${esc(o.image)}</td>
                        <td>
                            <span class="status ${stateClass(o.state)}">
                                ${labels[o.state] || esc(o.state)}
                            </span>
                        </td>
                        <td>${formatDate(o.createdAt)}</td>
                        <td>
                            <button
                                class="details-link"
                                data-order="${esc(o.orderId)}"
                            >
                                Подробнее
                            </button>
                        </td>
                    </tr>
                `)
                .join('')
            : `
                <tr>
                    <td colspan="6" class="empty">
                        Заказов пока нет. Создайте первую виртуальную машину выше.
                    </td>
                </tr>
            `;
    } catch (error) {
        toast(error.message);
    }
}

async function showOrder(id) {
    try {
        const o = await api(`/api/v1/orders/${encodeURIComponent(id)}`);

        $('#order-details').innerHTML = `
            <div class="eyebrow">КАРТОЧКА ЗАКАЗА</div>

            <h2 class="dialog-title">${esc(o.vmName)}</h2>

            <div class="dialog-id">${esc(o.orderId)}</div>

            <div style="margin-top: 14px">
                <span class="status ${stateClass(o.state)}">
                    ${labels[o.state] || esc(o.state)}
                </span>
            </div>

            <div class="detail-grid">
                <div class="detail-cell">
                    <span>Конфигурация</span>
                    <b>${esc(o.flavor.name)}</b>
                </div>

                <div class="detail-cell">
                    <span>Образ</span>
                    <b>${esc(o.image)}</b>
                </div>

                <div class="detail-cell">
                    <span>Процессор</span>
                    <b>${o.flavor.vcpu} vCPU</b>
                </div>

                <div class="detail-cell">
                    <span>Память / диск</span>
                    <b>
                        ${Math.round(o.flavor.memoryMb / 1024)} ГБ /
                        ${o.flavor.diskGb} ГБ
                    </b>
                </div>

                <div class="detail-cell">
                    <span>ID провайдера</span>
                    <b>
                        ${esc(o.providerVmId || 'Назначается при развертывании')}
                    </b>
                </div>

                <div class="detail-cell">
                    <span>Внутренний IP</span>
                    <b>${esc(o.privateIp || 'Пока не назначен')}</b>
                </div>

                <div class="detail-cell">
                    <span>Создана</span>
                    <b>${formatDate(o.createdAt)}</b>
                </div>
            </div>
        `;

        $('#order-dialog').showModal();
    } catch (error) {
        toast(error.message);
    }
}

function toast(message) {
    const node = $('#toast');

    node.textContent = message;
    node.classList.add('show');

    setTimeout(() => node.classList.remove('show'), 3200);
}

let authMode = 'login';

function setAuthMode(mode) {
    authMode = mode;

    const registering = mode === 'register';

    $('#display-wrap').hidden = !registering;

    $('#auth-eyebrow').textContent = registering
        ? 'НОВЫЙ АККАУНТ'
        : 'ДОБРО ПОЖАЛОВАТЬ';

    $('#auth-title').textContent = registering
        ? 'Создать аккаунт'
        : 'Войдите в VMaaS';

    $('#auth-copy').textContent = registering
        ? 'Для аккаунта будет создан личный проект с демонстрационной квотой.'
        : 'Войдите, чтобы управлять проектами и виртуальными машинами.';

    $('#auth-submit').innerHTML = registering
        ? 'Зарегистрироваться <span>→</span>'
        : 'Войти <span>→</span>';

    $('#auth-toggle').textContent = registering
        ? 'Уже есть аккаунт? Войти'
        : 'Создать аккаунт';

    $('.demo-hint').hidden = registering;
}

async function enterSession(session) {
    localStorage.setItem('vmaasToken', session.accessToken);

    scheduleRefresh();

    $('#auth-screen').hidden = true;
    $('#profile-name').textContent = session.user.displayName;
    $('#profile-email').textContent = session.user.email;

    await loadCatalog();
    await loadOrders();
}

function scheduleRefresh() {
    clearTimeout(refreshTimer);

    refreshTimer = setTimeout(async () => {
        try {
            const renewed = await api('/api/v1/auth/refresh', {
                method: 'POST',
                body: '{}'
            });

            localStorage.setItem('vmaasToken', renewed.accessToken);

            scheduleRefresh();
        } catch {
            localStorage.removeItem('vmaasToken');
            $('#auth-screen').hidden = false;
        }
    }, 7 * 60 * 60 * 1000);
}

$('#auth-toggle').addEventListener('click', () => {
    setAuthMode(authMode === 'login' ? 'register' : 'login');
});

$('#auth-form').addEventListener('submit', async (e) => {
    e.preventDefault();

    const message = $('#auth-message');

    message.hidden = true;

    const input = {
        email: $('#auth-email').value.trim(),
        password: $('#auth-password').value
    };

    if (authMode === 'register') {
        input.displayName = $('#display-name').value.trim();
    }

    try {
        const session = await api(`/api/v1/auth/${authMode}`, {
            method: 'POST',
            body: JSON.stringify(input)
        });

        await enterSession(session);
    } catch (error) {
        message.textContent = error.message;
        message.hidden = false;
    }
});

$('#logout').addEventListener('click', async () => {
    try {
        await api('/api/v1/auth/logout', {
            method: 'POST',
            body: '{}'
        });
    } catch { }

    clearTimeout(refreshTimer);

    localStorage.removeItem('vmaasToken');

    $('#auth-screen').hidden = false;

    setAuthMode('login');
});

$('#flavor').addEventListener('change', renderFlavor);

$('#refresh').addEventListener('click', loadOrders);

$('#orders-body').addEventListener('click', (e) => {
    const button = e.target.closest('[data-order]');

    if (button) {
        showOrder(button.dataset.order);
    }
});

$('.dialog-close').addEventListener('click', () => {
    $('#order-dialog').close();
});

$('#order-dialog').addEventListener('click', (e) => {
    if (e.target === $('#order-dialog')) {
        $('#order-dialog').close();
    }
});

$('#order-form').addEventListener('submit', async (e) => {
    e.preventDefault();

    const button = $('#submit-order');
    const message = $('#form-message');

    button.disabled = true;
    button.textContent = 'Отправляем…';

    message.hidden = true;

    try {
        const result = await api('/api/v1/orders', {
            method: 'POST',
            body: JSON.stringify({
                projectId: Number($('#project').value),
                imageId: Number($('#image').value),
                flavorId: Number($('#flavor').value),
                vmName: $('#vm-name').value.trim(),
                idempotencyKey: crypto.randomUUID()
            })
        });

        message.className = 'message success';
        message.textContent = `Заказ принят: ${result.orderId}. Развертывание началось.`;
        message.hidden = false;

        $('#vm-name').value = '';

        await loadOrders();

        $('#orders').scrollIntoView({
            behavior: 'smooth'
        });
    } catch (error) {
        message.className = 'message error';
        message.textContent = error.message;
        message.hidden = false;
    } finally {
        button.disabled = false;
        button.innerHTML = 'Заказать VM ';
    }
});

(async () => {
    try {
        const runtime = await api('/api/v1/runtime');
        const real = runtime.computeProvider === 'openstack';

        // $('#provider-mode').textContent = real
        //     ? 'OpenStack Compute'
        //     : 'Mock-провайдер';

        // $('#provider-note').textContent = real
        //     ? 'Провайдер: OpenStack'
        //     : 'VM эмулируется mock-провайдером';
    } catch { }

    const token = localStorage.getItem('vmaasToken');

    if (!token) {
        $('#auth-screen').hidden = false;
        return;
    }

    try {
        const me = await api('/api/v1/auth/me');

        $('#profile-name').textContent = me.user.displayName;
        $('#profile-email').textContent = me.user.email;
        $('#auth-screen').hidden = true;

        scheduleRefresh();

        await loadCatalog();
        await loadOrders();
    } catch (error) {
        localStorage.removeItem('vmaasToken');

        $('#auth-screen').hidden = false;

        toast(error.message);
    }

    setInterval(() => {
        if (localStorage.getItem('vmaasToken')) {
            loadOrders();
        }
    }, 3000);
})();
