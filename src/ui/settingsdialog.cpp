#include "settingsdialog.h"

#include "progressitemdelegate.h"
#include "ui_settingsdialog.h"
#include "core/application.h"
#include "core/settings.h"
#include "registry/docsetregistry.h"
#include "registry/listmodel.h"

#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QMessageBox>
#include <QTemporaryFile>

#include <QtConcurrent/QtConcurrent>

using namespace Zeal;

namespace {
const char *ApiUrl = "http://api.zealdocs.org";

// QNetworkReply properties
const char *DocsetMetadataProperty = "docsetMetadata";
const char *DownloadTypeProperty = "downloadType";
const char *ListItemIndexProperty = "listItem";
}

SettingsDialog::SettingsDialog(Core::Application *app, ListModel *listModel, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SettingsDialog()),
    m_application(app),
    m_docsetRegistry(app->docsetRegistry()),
    m_zealListModel(listModel)
{
    ui->setupUi(this);

    ui->downloadableGroup->hide();
    ui->docsetsProgress->hide();

    ui->listView->setModel(m_zealListModel);

    ProgressItemDelegate *progressDelegate = new ProgressItemDelegate();
    ui->docsetsList->setItemDelegate(progressDelegate);
    ui->listView->setItemDelegate(progressDelegate);

    // Setup signals & slots
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &SettingsDialog::saveSettings);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &SettingsDialog::loadSettings);
    connect(ui->buttonBox, &QDialogButtonBox::clicked, this, [this](QAbstractButton *button) {
        if (button == ui->buttonBox->button(QDialogButtonBox::Apply))
            saveSettings();
    });

    connect(ui->minFontSize, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SettingsDialog::minFontSizeChanged);

    connect(ui->addFeedButton, &QPushButton::clicked, this, &SettingsDialog::addDashFeed);
    connect(ui->downloadButton, &QPushButton::clicked, this, &SettingsDialog::downloadDocsetList);
    connect(ui->updateButton, &QPushButton::clicked, this, &SettingsDialog::updateFeedDocsets);

    connect(m_application, &Core::Application::extractionCompleted,
            this, &SettingsDialog::extractionCompleted);
    connect(m_application, &Core::Application::extractionError,
            this, &SettingsDialog::extractionError);

    loadSettings();
}

SettingsDialog::~SettingsDialog()
{
    delete ui;
}

void SettingsDialog::extractionCompleted(const QString &filePath)
{
    QString docsetName;

    /// FIXME: Come up with a better approach
    for (const QString &key : m_tmpFiles.keys()) {
        if (m_tmpFiles[key]->fileName() == filePath) {
            docsetName = key;
            break;
        }
    }

    const QDir dataDir(m_application->settings()->docsetPath);
    const QString docsetPath = dataDir.absoluteFilePath(docsetName + QStringLiteral(".docset"));


    // Write metadata about docset
    DocsetMetadata metadata = m_availableDocsets.contains(docsetName)
            ? m_availableDocsets[docsetName]
            : m_userFeeds[docsetName];
    metadata.toFile(docsetPath + QStringLiteral("/meta.json"));

    QMetaObject::invokeMethod(m_docsetRegistry, "addDocset", Qt::BlockingQueuedConnection,
                              Q_ARG(QString, docsetPath));

    m_zealListModel->resetModulesCounts();
    emit refreshRequested();
    ui->listView->reset();

    QListWidgetItem *listItem = findDocsetListItem(metadata.title());
    if (listItem) {
        listItem->setData(ZealDocsetDoneInstalling, true);
        listItem->setData(ProgressItemDelegate::ProgressFormatRole, "Done");
        listItem->setData(ProgressItemDelegate::ProgressRole, 1);
        listItem->setData(ProgressItemDelegate::ProgressMaxRole, 1);
    }
    endTasks();
    delete m_tmpFiles.take(docsetName);
}

void SettingsDialog::extractionError(const QString &filePath, const QString &errorString)
{
    QString docsetName = QFileInfo(filePath).baseName() + QStringLiteral(".docset");
    QMessageBox::warning(this, QStringLiteral("Extraction Error"),
                         QString(QStringLiteral("Cannot extract docset '%1': %2")).arg(docsetName).arg(errorString));
    delete m_tmpFiles.take(docsetName);
}

/*!
  \internal
  Should be connected to all \l QNetworkReply::finished signals in order to process possible
  HTTP-redirects correctly.
*/
void SettingsDialog::downloadCompleted()
{
    QScopedPointer<QNetworkReply, QScopedPointerDeleteLater> reply(
                qobject_cast<QNetworkReply *>(sender()));

    replies.removeOne(reply.data());

    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() != QNetworkReply::OperationCanceledError)
            QMessageBox::warning(this, QStringLiteral("Network Error"), reply->errorString());

        return;
    }

    QUrl redirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (redirectUrl.isValid()) {
        if (redirectUrl.isRelative())
            redirectUrl = reply->request().url().resolved(redirectUrl);

        /// TODO: Verify if scheme can be missing
        if (redirectUrl.scheme().isEmpty())
            redirectUrl.setScheme(reply->request().url().scheme());

        QNetworkReply *newReply = startDownload(redirectUrl);

        // Copy properties
        newReply->setProperty(DocsetMetadataProperty, reply->property(DocsetMetadataProperty));
        newReply->setProperty(DownloadTypeProperty, reply->property(DownloadTypeProperty));
        newReply->setProperty(ListItemIndexProperty, reply->property(ListItemIndexProperty));

        connect(newReply, &QNetworkReply::finished, this, &SettingsDialog::downloadCompleted);

        return;
    }

    switch (static_cast<DownloadType>(reply->property(DownloadTypeProperty).toUInt())) {
    case DownloadDocsetList: {
        QJsonParseError jsonError;
        const QJsonDocument jsonDoc = QJsonDocument::fromJson(reply->readAll(), &jsonError);

        if (jsonError.error != QJsonParseError::NoError) {
            QMessageBox::warning(this, QStringLiteral("Error"),
                                 QStringLiteral("Corrupted docset list: ")
                                 + jsonError.errorString());
            break;
        }

        processDocsetList(jsonDoc.array());

        if (!m_availableDocsets.isEmpty())
            ui->downloadableGroup->show();

        endTasks();
        break;
    }

    case DownloadDashFeed: {
        DocsetMetadata metadata = DocsetMetadata::fromDashFeed(reply->request().url(), reply->readAll());
        DocsetMetadata oldMetadata;

        if (metadata.urls().isEmpty()) {
            QMessageBox::critical(this, "Zeal", "Could not read docset feed!");
            break;
        }

        QVariant oldMeta = reply->property(DocsetMetadataProperty);
        if (oldMeta.isValid())
            oldMetadata = oldMeta.value<DocsetMetadata>();

        /// TODO: Check revision
        if (metadata.version().isEmpty() || oldMetadata.version() != metadata.version()) {
            m_userFeeds[metadata.name()] = metadata;
            QNetworkReply *newReply = startDownload(metadata.url());
            newReply->setProperty(DocsetMetadataProperty, QVariant::fromValue(metadata));
            newReply->setProperty(DownloadTypeProperty, DownloadDocset);
            connect(newReply, &QNetworkReply::finished, this, &SettingsDialog::downloadCompleted);
        }
        break;
    }

    case DownloadDocset: {
        const DocsetMetadata metadata = reply->property(DocsetMetadataProperty).value<DocsetMetadata>();

        QTemporaryFile *tmpFile = new QTemporaryFile();
        tmpFile->open();
        qint64 readBytes = 1;
        const int BUFSIZE = 1024*1024;
        char buf[BUFSIZE];
        while (readBytes > 0) {
            readBytes = reply->read(buf, BUFSIZE);
            if (readBytes > 0)
                tmpFile->write(buf, readBytes);
        }
        tmpFile->close();

        m_tmpFiles.insert(metadata.name(), tmpFile);
        m_application->extract(tmpFile->fileName(), m_application->settings()->docsetPath,
                               metadata.name() + QStringLiteral(".docset"));
        break;
    }
    }

    // If all enqueued downloads have finished executing
    if (replies.isEmpty())
        resetProgress();
}

void SettingsDialog::loadSettings()
{
    const Core::Settings * const settings = m_application->settings();
    // General Tab
    ui->startMinimizedCheckBox->setChecked(settings->startMinimized);

    ui->systrayGroupBox->setChecked(settings->showSystrayIcon);
    ui->minimizeToSystrayCheckBox->setChecked(settings->minimizeToSystray);
    ui->hideToSystrayCheckBox->setChecked(settings->hideOnClose);

    ui->toolButton->setKeySequence(settings->showShortcut);

    //
    ui->minFontSize->setValue(settings->minimumFontSize);
    ui->storageEdit->setText(QDir::toNativeSeparators(settings->docsetPath));

    // Network Tab
    switch (settings->proxyType) {
    case Core::Settings::ProxyType::None:
        ui->noProxySettings->setChecked(true);
        break;
    case Core::Settings::ProxyType::System:
        ui->systemProxySettings->setChecked(true);
        break;
    case Core::Settings::ProxyType::UserDefined:
        ui->manualProxySettings->setChecked(true);
        ui->httpProxy->setText(settings->proxyHost);
        ui->httpProxyPort->setValue(settings->proxyPort);
        ui->httpProxyNeedsAuth->setChecked(settings->proxyAuthenticate);
        ui->httpProxyUser->setText(settings->proxyUserName);
        ui->httpProxyPass->setText(settings->proxyPassword);
        break;
    }
}

// creates a total download progress for multiple QNetworkReplies
void SettingsDialog::on_downloadProgress(quint64 received, quint64 total)
{
    // Don't show progress for non-docset pages
    if (received < 10240)
        return;

    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());

    QPair<qint32, qint32> *previousProgress = progress[reply];
    if (previousProgress == nullptr) {
        previousProgress = new QPair<qint32, qint32>(0, 0);
        progress[reply] = previousProgress;
    }

    // Try to get the item associated to the request
    QListWidgetItem *item = ui->docsetsList->item(reply->property(ListItemIndexProperty).toInt());
    if (item) {
        item->setData(ProgressItemDelegate::ProgressMaxRole, total);
        item->setData(ProgressItemDelegate::ProgressRole, received);
    }

    currentDownload += received - previousProgress->first;
    totalDownload += total - previousProgress->second;
    previousProgress->first = received;
    previousProgress->second = total;
    displayProgress();
}

void SettingsDialog::displayProgress()
{
    ui->docsetsProgress->setValue(currentDownload);
    ui->docsetsProgress->setMaximum(totalDownload);
    ui->docsetsProgress->setVisible(tasksRunning > 0);
}

void SettingsDialog::startTasks(qint8 tasks)
{
    tasksRunning += tasks;
    if (!tasksRunning)
        resetProgress();

    displayProgress();
}

void SettingsDialog::endTasks(qint8 tasks)
{
    startTasks(-tasks);

    if (tasksRunning > 0)
        return;

    // Remove completed items
    for (int i = ui->docsetsList->count() - 1; i >= 0; --i) {
        QListWidgetItem *item = ui->docsetsList->item(i);
        if (item->data(ZealDocsetDoneInstalling).toBool()) {
            item->setCheckState(Qt::Unchecked);
            item->setHidden(true);
            item->setData(ProgressItemDelegate::ProgressVisibleRole, false);
            item->setData(ZealDocsetDoneInstalling, false);
            item->setData(ProgressItemDelegate::ProgressFormatRole, QVariant());
            item->setData(ProgressItemDelegate::ProgressRole, QVariant());
            item->setData(ProgressItemDelegate::ProgressMaxRole, QVariant());
        }
    }
}

void SettingsDialog::updateFeedDocsets()
{
    ui->downloadableGroup->show();
    bool missingMetadata = false;

    for (const Docset &docset : m_docsetRegistry->docsets()) {
        const DocsetMetadata metadata = docset.metadata;
        if (metadata.source().isEmpty())
            missingMetadata = true;

        const QUrl feedUrl = metadata.feedUrl();
        // Skip not manually added feeds
        if (feedUrl.isEmpty())
            continue;

        QNetworkReply *reply = startDownload(feedUrl);
        reply->setProperty(DownloadTypeProperty, DownloadDashFeed);

        QListWidgetItem *listItem = findDocsetListItem(metadata.title());
        if (listItem)
            reply->setProperty(ListItemIndexProperty, ui->docsetsList->row(listItem));

        reply->setProperty(DocsetMetadataProperty, QVariant::fromValue(metadata));
        connect(reply, &QNetworkReply::finished, this, &SettingsDialog::downloadCompleted);
    }

    if (!missingMetadata)
        return;

    int r = QMessageBox::information(this, "Zeal",
                                     "Some docsets are missing metadata, would you like to redownload all docsets with missing metadata?",
                                     QMessageBox::Yes | QMessageBox::No);
    if (r == QMessageBox::No)
        return;

    if (m_availableDocsets.isEmpty())
        downloadDocsetList();

    // There must be a better way to do this.
    QFuture<void> future = QtConcurrent::run([=] {
        while (m_availableDocsets.isEmpty() || replies.size())
            QThread::yieldCurrentThread();
    });

    QFutureWatcher<void> *watcher = new QFutureWatcher<void>;
    watcher->setFuture(future);
    connect(watcher, &QFutureWatcher<void>::finished, [=] {
        for (const Docset &docset : m_docsetRegistry->docsets()) {
            if (!docset.metadata.source().isEmpty() && m_availableDocsets.contains(docset.name()))
                downloadDashDocset(docset.name());
        }
    });
}

void SettingsDialog::processDocsetList(const QJsonArray &list)
{
    for (const QJsonValue &v : list) {
        QJsonObject docsetJson = v.toObject();
        QString source = QStringLiteral("source");
        docsetJson[source] = QStringLiteral("kapeli");

        DocsetMetadata metadata(docsetJson);
        m_availableDocsets.insert(metadata.name(), metadata);
    }

    /// TODO: Move into a dedicated method
    for (const DocsetMetadata &metadata : m_availableDocsets) {
        const QIcon icon(QString(QStringLiteral("icons:%1.png")).arg(metadata.icon()));

        QListWidgetItem *listItem = new QListWidgetItem(icon, metadata.title(), ui->docsetsList);
        listItem->setData(ListModel::DocsetNameRole, metadata.name());
        listItem->setCheckState(Qt::Unchecked);

        if (m_docsetRegistry->contains(metadata.name()))
            listItem->setHidden(true);
    }
}

void SettingsDialog::downloadDashDocset(const QString &name)
{
    /// TODO: Select fastest mirror
    static const QStringList kapeliUrls = {
        QStringLiteral("http://sanfrancisco.kapeli.com"),
        QStringLiteral("http://sanfrancisco2.kapeli.com"),
        QStringLiteral("http://london.kapeli.com"),
        QStringLiteral("http://london2.kapeli.com"),
        QStringLiteral("http://london3.kapeli.com"),
        QStringLiteral("http://newyork.kapeli.com"),
        QStringLiteral("http://newyork2.kapeli.com"),
        QStringLiteral("http://sydney.kapeli.com"),
        QStringLiteral("http://tokyo.kapeli.com"),
        QStringLiteral("http://tokyo2.kapeli.com")
    };

    if (!m_availableDocsets.contains(name))
        return;

    const QUrl url = QString(QStringLiteral("%1/feeds/%2.tgz"))
            .arg(kapeliUrls.at(qrand() % kapeliUrls.size()))
            .arg(name);

    QNetworkReply *reply = startDownload(url);
    reply->setProperty(DocsetMetadataProperty, QVariant::fromValue(m_availableDocsets[name]));
    reply->setProperty(DownloadTypeProperty, DownloadDocset);
    reply->setProperty(ListItemIndexProperty,
                       ui->docsetsList->row(findDocsetListItem(m_availableDocsets[name].title())));

    connect(reply, &QNetworkReply::finished, this, &SettingsDialog::downloadCompleted);
}

void SettingsDialog::downloadDocsetList()
{
    ui->downloadButton->hide();
    ui->docsetsList->clear();
    m_availableDocsets.clear();

    QNetworkReply *reply = startDownload(QUrl(ApiUrl + QStringLiteral("/docsets")));
    reply->setProperty(DownloadTypeProperty, DownloadDocsetList);
    connect(reply, &QNetworkReply::finished, this, &SettingsDialog::downloadCompleted);
}

void SettingsDialog::on_docsetsList_itemSelectionChanged()
{
    ui->downloadDocsetButton->setEnabled(ui->docsetsList->selectedItems().count() > 0);
}

void SettingsDialog::on_downloadDocsetButton_clicked()
{
    if (!replies.isEmpty()) {
        stopDownloads();
        return;
    }

    // Find each checked item, and create a NetworkRequest for it.
    for (int i = 0; i < ui->docsetsList->count(); ++i) {
        QListWidgetItem *item = ui->docsetsList->item(i);
        if (item->checkState() != Qt::Checked)
            continue;

        item->setData(ProgressItemDelegate::ProgressVisibleRole, true);
        item->setData(ProgressItemDelegate::ProgressRole, 0);
        item->setData(ProgressItemDelegate::ProgressMaxRole, 1);

        downloadDashDocset(item->data(ListModel::DocsetNameRole).toString());
    }

    if (replies.count() > 0)
        ui->downloadDocsetButton->setText("Stop downloads");
}

void SettingsDialog::on_storageButton_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(0, "Open Directory");
    if (!dir.isEmpty())
        ui->storageEdit->setText(QDir::toNativeSeparators(dir));

}

void SettingsDialog::on_deleteButton_clicked()
{
    const QString docsetTitle = ui->listView->currentIndex().data().toString();
    const int answer
            = QMessageBox::question(this, tr("Remove Docset"),
                                    QString("Do you want to permanently delete the '%1' docset? ")
                                    .arg(docsetTitle));
    if (answer == QMessageBox::No)
        return;

    const QDir dataDir(m_application->settings()->docsetPath);
    const QString docsetName = ui->listView->currentIndex().data(ListModel::DocsetNameRole).toString();
    m_zealListModel->removeRow(ui->listView->currentIndex().row());
    if (dataDir.exists()) {
        ui->docsetsProgress->show();
        ui->deleteButton->hide();
        startTasks();
        QFuture<bool> future = QtConcurrent::run([=] {
            QDir docsetDir(dataDir);
            return !docsetDir.cd(docsetName + ".docset") || docsetDir.removeRecursively();
        });
        QFutureWatcher<bool> *watcher = new QFutureWatcher<bool>();
        watcher->setFuture(future);
        connect(watcher, &QFutureWatcher<void>::finished, [=] {
            if (!watcher->result()) {
                QMessageBox::warning(this, QStringLiteral("Error"),
                                     QString("Cannot delete docset <strong>%1</strong>!").arg(docsetTitle));
            }

            endTasks();
            ui->deleteButton->show();

            QListWidgetItem *listItem = findDocsetListItem(docsetTitle);
            if (listItem)
                listItem->setHidden(false);

            watcher->deleteLater();
        });
    }
}

void SettingsDialog::on_listView_clicked(const QModelIndex &index)
{
    Q_UNUSED(index)
    ui->deleteButton->setEnabled(true);
}

void SettingsDialog::resetProgress()
{
    progress.clear();
    totalDownload = 0;
    currentDownload = 0;
    ui->downloadButton->setVisible(m_availableDocsets.isEmpty());
    ui->downloadDocsetButton->setText("Download");
    ui->downloadButton->setEnabled(true);
    ui->updateButton->setEnabled(true);
    ui->addFeedButton->setEnabled(true);
    ui->docsetsList->setEnabled(true);
    displayProgress();
}

QNetworkReply *SettingsDialog::startDownload(const QUrl &url)
{
    startTasks(1);

    QNetworkReply *reply = m_application->download(url);
    connect(reply, &QNetworkReply::downloadProgress, this, &SettingsDialog::on_downloadProgress);
    replies.append(reply);

    ui->downloadDocsetButton->setText("Stop downloads");
    ui->downloadButton->setEnabled(false);
    ui->updateButton->setEnabled(false);
    ui->addFeedButton->setEnabled(false);

    return reply;
}

void SettingsDialog::stopDownloads()
{
    for (QNetworkReply *reply: replies) {
        // Hide progress bar
        QListWidgetItem *listItem = ui->docsetsList->item(reply->property(ListItemIndexProperty).toInt());
        if (!listItem)
            continue;

        listItem->setData(ProgressItemDelegate::ProgressVisibleRole, false);
        reply->abort();
    }
}

void SettingsDialog::saveSettings()
{
    Core::Settings * const settings = m_application->settings();
    // General Tab
    settings->startMinimized = ui->startMinimizedCheckBox->isChecked();

    settings->showSystrayIcon = ui->systrayGroupBox->isChecked();
    settings->minimizeToSystray = ui->minimizeToSystrayCheckBox->isChecked();
    settings->hideOnClose = ui->hideToSystrayCheckBox->isChecked();

    settings->showShortcut = ui->toolButton->keySequence();

    //
    settings->minimumFontSize = ui->minFontSize->text().toInt();

    if (QDir::fromNativeSeparators(ui->storageEdit->text()) != settings->docsetPath) {
        settings->docsetPath = QDir::fromNativeSeparators(ui->storageEdit->text());
        m_docsetRegistry->initialiseDocsets(settings->docsetPath);
        emit refreshRequested();
    }

    // Network Tab
    // Proxy settings
    if (ui->noProxySettings->isChecked())
        settings->proxyType = Core::Settings::ProxyType::None;
    else if (ui->systemProxySettings->isChecked())
        settings->proxyType = Core::Settings::ProxyType::System;
    else if (ui->manualProxySettings->isChecked())
        settings->proxyType = Core::Settings::ProxyType::UserDefined;

    settings->proxyHost = ui->httpProxy->text();
    settings->proxyPort = ui->httpProxyPort->text().toUInt();
    settings->proxyAuthenticate = ui->httpProxyNeedsAuth->isChecked();
    settings->proxyUserName = ui->httpProxyUser->text();
    settings->proxyPassword = ui->httpProxyPass->text();

    settings->save();

    emit webPageStyleUpdated();
}

void SettingsDialog::on_tabWidget_currentChanged(int current)
{
    if (ui->tabWidget->widget(current) != ui->docsetsTab)
        return;

    // Ensure the list is completely up to date
    QModelIndex index = ui->listView->currentIndex();
    ui->listView->reset();

    if (index.isValid())
        ui->listView->setCurrentIndex(index);

    if (!ui->docsetsList->count())
        downloadDocsetList();
}

void SettingsDialog::addDashFeed()
{
    QString txt = QApplication::clipboard()->text();
    if (!txt.startsWith(QStringLiteral("dash-feed://")))
        txt.clear();

    QString feedUrl = QInputDialog::getText(this, "Zeal", "Feed URL:", QLineEdit::Normal, txt);
    if (feedUrl.isEmpty())
        return;

    if (feedUrl.startsWith(QStringLiteral("dash-feed://"))) {
        feedUrl = feedUrl.remove(0, 12);
        feedUrl = QUrl::fromPercentEncoding(feedUrl.toUtf8());
    }

    QNetworkReply *reply = startDownload(feedUrl);
    reply->setProperty(DownloadTypeProperty, DownloadDashFeed);
    connect(reply, &QNetworkReply::finished, this, &SettingsDialog::downloadCompleted);
}

QListWidgetItem *SettingsDialog::findDocsetListItem(const QString &title) const
{
    const QList<QListWidgetItem *> items
            = ui->docsetsList->findItems(title, Qt::MatchFixedString);

    if (items.isEmpty())
        return nullptr;

    return items.first();
}
