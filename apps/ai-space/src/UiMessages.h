#pragma once
#include <QMessageBox>
namespace AiSpaceUi {
QMessageBox::StandardButton smartQuestion(QWidget *parent,const QString &title,const QString &text,
    QMessageBox::StandardButtons buttons,QMessageBox::StandardButton defaultButton);
void smartInformation(QWidget *parent,const QString &title,const QString &text);
void smartWarning(QWidget *parent,const QString &title,const QString &text);
}
