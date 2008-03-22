/*
    Copyright (C) 2007 by Robert Knight <robertknight@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA.
*/

// Own
#include "ScreenWindow.h"

// Qt
#include <KDebug>
#include <QStack>

// Konsole
#include "Screen.h"
#include "TerminalCharacterDecoder.h"

using namespace Konsole;

ScreenWindow::ScreenWindow(QObject* parent)
    : QObject(parent)
	, _windowBuffer(0)
	, _windowBufferSize(0)
	, _bufferNeedsUpdate(true)
	, _windowLines(1)
    , _currentLine(0)
    , _trackOutput(true)
    , _scrollCount(0)
{
	_filterData.enabled = true;
	_filterData.visibleLines = 0;
}
ScreenWindow::~ScreenWindow()
{
	delete[] _windowBuffer;
}
void ScreenWindow::setScreen(Screen* screen)
{
    Q_ASSERT( screen );

    _screen = screen;
}

Screen* ScreenWindow::screen() const
{
    return _screen;
}

ScreenWindow::FoldType ScreenWindow::foldType(int line) const
{
	if (!_filterData.enabled)
		return FoldNone;

	const int screenLine = line;
	
	if (_filterData.foldStarts.testBit(screenLine))
		return FoldStart;
	else if (_filterData.foldEnds.testBit(screenLine))
		return FoldEnd;
	else
		return FoldNone;
}
bool ScreenWindow::isFoldOpen(int line) const
{
	Q_ASSERT(foldType(line) == FoldStart);

	return _filterData.expanded.testBit(line);
}
void ScreenWindow::setFoldOpen(int line,bool open)
{
	Q_ASSERT(foldType(line) == FoldStart);
	
	updateFilterDataSize();

	_filterData.expanded.setBit(line,open);
}
void ScreenWindow::setFold(int startLine,int endLine,bool fold)
{
	Q_ASSERT(startLine <= endLine);
	
	_filterData.enabled = true;

	updateFilterDataSize();

	// check that fold status makes sense
	Q_ASSERT(_filterData.foldStarts.testBit(startLine) ==
			 _filterData.foldEnds.testBit(endLine));	

	//qDebug() << (fold ? "Creating fold" : "Removing fold") << "from line" << startLine << "to" << endLine; 

	_filterData.foldStarts.setBit(startLine,fold);
	_filterData.foldEnds.setBit(endLine,fold);
	_filterData.expanded.setBit(startLine,false);

}
void ScreenWindow::removeAllFolds()
{
	_filterData.foldStarts.fill(false);
	_filterData.foldEnds.fill(false);
	_filterData.filteredLines.fill(true);
	_filterData.expanded.fill(false);

	_filterData.enabled = false;
}
void ScreenWindow::updateFilter()
{
	updateFilterDataSize();

	int count = lineCount();

	// mark all lines as visible
	_filterData.filteredLines.fill(true);

	// stores status of folds covering current line
	// contains 'true' for open folds, 'false' for closed folds
	QStack<bool> foldStack;

	bool stackHasClosedFold = false;

	for (int i=0;i<count;i++)
	{
		_filterData.filteredLines[i] = !stackHasClosedFold;
		
		if (_filterData.foldStarts.testBit(i))
		{
			bool open = _filterData.expanded.testBit(i);
			foldStack.push(open);
			if (!open)
				stackHasClosedFold = true;
		}

		if (_filterData.foldEnds.testBit(i))
		{
			foldStack.pop();
			stackHasClosedFold = foldStack.contains(false);
		}
	}

	// check that count of start/end folds are the same
	Q_ASSERT(foldStack.isEmpty());

	_filterData.visibleLines = _filterData.filteredLines.count(true);
}

void ScreenWindow::getFilteredImage(Character* buffer,int size,int startLine,int endLine)
{
	//qDebug() << "Filtered image from" << startLine << "to" << endLine;

	Q_ASSERT( startLine <= endLine );
	Q_ASSERT( size >= (startLine-endLine+1)*windowColumns() );

	updateFilter();

	// find first line to copy
	int startFrom = 0;
	int visibleLineCount = 0;
	int cursorLine = _screen->getHistLines() + _screen->getCursorY();
	for (int i=0;i<lineCount();i++)
	{
		if (_filterData.filteredLines[i] || i == cursorLine)
			visibleLineCount++;

		if (visibleLineCount > startLine)
			break;
		
		startFrom++;
	}

	Q_ASSERT( startFrom >= startLine );

	// copy visible lines
	int count = 0;
	for (int i=startFrom ; count < (endLine-startLine+1) && i < lineCount() ; i++)
	{
		if (_filterData.filteredLines[i] || i == cursorLine)
		{
			Character cbuffer[windowColumns()];
			_screen->getImage(cbuffer,windowColumns(),i,i);
			QString text;
			for (int j=0;j<windowColumns();j++)
				text[j] = cbuffer[j].character;

			//qDebug() << "Showing line" << i << text.simplified() <<
			//"is filtered" << _filterData.filteredLines[i];

			_screen->getImage(buffer + (windowColumns()*count), windowColumns() ,i,i);
			visibleLineCount++;
			count++;
		}
	}

	// fill unused area
	int charsToFill = (windowLines() - count) * windowColumns();

	Q_ASSERT( (windowColumns()*count + charsToFill) <= size );

	Screen::fillWithDefaultChar(_windowBuffer + (windowColumns()*count) , charsToFill);
}

Character* ScreenWindow::getImage()
{
	// reallocate internal buffer if the window size has changed
	int size = windowLines() * windowColumns();
	if (_windowBuffer == 0 || _windowBufferSize != size) 
	{
		delete[] _windowBuffer;
		_windowBufferSize = size;
		_windowBuffer = new Character[size];
		_bufferNeedsUpdate = true;
	}

	 if (!_bufferNeedsUpdate)
		return _windowBuffer;

	// iterate through lines and copy visible ones if filters are enabled,
	// otherwise fill the whole window buffer from the screen in one call
	if (_filterData.enabled)
	{
		getFilteredImage(_windowBuffer,size,
						  currentLine(),endWindowLine());
	}
	else
	{
		_screen->getImage(_windowBuffer,size,currentLine(),endWindowLine());
	}

	// this window may look beyond the end of the screen, in which 
	// case there will be an unused area which needs to be filled
	// with blank characters
	fillUnusedArea();

	_bufferNeedsUpdate = false;
	return _windowBuffer;
}

void ScreenWindow::fillUnusedArea()
{
	int screenEndLine = _screen->getHistLines() + _screen->getLines() - 1;
	int windowEndLine = currentLine() + windowLines() - 1;

	int unusedLines = windowEndLine - screenEndLine;
	int charsToFill = unusedLines * windowColumns();

	Screen::fillWithDefaultChar(_windowBuffer + _windowBufferSize - charsToFill,charsToFill); 
}

// return the index of the line at the end of this window, or if this window 
// goes beyond the end of the screen, the index of the line at the end
// of the screen.
//
// when passing a line number to a Screen method, the line number should
// never be more than endWindowLine()
//
int ScreenWindow::endWindowLine() const
{
	return qMin(currentLine() + windowLines() - 1,
				lineCount() - 1);
}
QVector<LineProperty> ScreenWindow::getLineProperties()
{
    QVector<LineProperty> result = _screen->getLineProperties(currentLine(),endWindowLine());
	
	if (result.count() != windowLines())
		result.resize(windowLines());

	return result;
}

QString ScreenWindow::selectedText( bool preserveLineBreaks ) const
{
    return _screen->selectedText( preserveLineBreaks );
}

void ScreenWindow::getSelectionStart( int& column , int& line )
{
    _screen->getSelectionStart(column,line);
    line -= currentLine();
}
void ScreenWindow::getSelectionEnd( int& column , int& line )
{
    _screen->getSelectionEnd(column,line);
    line -= currentLine();
}
void ScreenWindow::setSelectionStart( int column , int line , bool columnMode )
{
    _screen->setSelectionStart( column , mapToScreen(line)  , columnMode);
	
	_bufferNeedsUpdate = true;
    emit selectionChanged();
}
int ScreenWindow::mapToScreen(int line) const
{
	return qMin(line+currentLine(),endWindowLine());
}
void ScreenWindow::setSelectionEnd( int column , int line )
{
    _screen->setSelectionEnd( column , mapToScreen(line) );

	_bufferNeedsUpdate = true;
    emit selectionChanged();
}

bool ScreenWindow::isSelected( int column , int line )
{
    return _screen->isSelected( column , mapToScreen(line) );
}

void ScreenWindow::clearSelection()
{
    _screen->clearSelection();

    emit selectionChanged();
}

void ScreenWindow::setWindowLines(int lines)
{
	Q_ASSERT(lines > 0);
	_windowLines = lines;
}
int ScreenWindow::windowLines() const
{
	return _windowLines;		
}

int ScreenWindow::windowColumns() const
{
    return _screen->getColumns();
}
int ScreenWindow::visibleLineCount() const
{
	if (_filterData.enabled)
		return _filterData.visibleLines;
	else
		return lineCount();
}
int ScreenWindow::lineCount() const
{
    return _screen->getHistLines() + _screen->getLines();
}
void ScreenWindow::updateFilterDataSize()
{
	if (!_filterData.enabled)
		return;

	int size = lineCount();

	if (_filterData.foldStarts.size() != size)
	{
		_filterData.foldStarts.resize(size);
		_filterData.foldEnds.resize(size);
		_filterData.filteredLines.fill(true,size);
		_filterData.expanded.resize(size);
	}
}
int ScreenWindow::columnCount() const
{
    return _screen->getColumns();
}

QPoint ScreenWindow::cursorPosition() const
{
    QPoint position;
    
    position.setX( _screen->getCursorX() );
    position.setY( _screen->getCursorY() );

    return position; 
}

int ScreenWindow::currentLine() const
{
    return qBound(0,_currentLine,visibleLineCount()-windowLines());
}

void ScreenWindow::scrollBy( RelativeScrollMode mode , int amount )
{
    if ( mode == ScrollLines )
    {
        scrollTo( currentLine() + amount );
    }
    else if ( mode == ScrollPages )
    {
        scrollTo( currentLine() + amount * ( windowLines() / 2 ) ); 
    }
}

bool ScreenWindow::atEndOfOutput() const
{
    return currentLine() == (lineCount()-windowLines());
}

void ScreenWindow::scrollTo( int line )
{
	int maxCurrentLineNumber = lineCount() - windowLines();
	line = qBound(0,line,maxCurrentLineNumber);

    const int delta = line - _currentLine;
    _currentLine = line;

    // keep track of number of lines scrolled by,
    // this can be reset by calling resetScrollCount()
    _scrollCount += delta;

	_bufferNeedsUpdate = true;

    emit scrolled(_currentLine);
}

void ScreenWindow::setTrackOutput(bool trackOutput)
{
    _trackOutput = trackOutput;
}

bool ScreenWindow::trackOutput() const
{
    return _trackOutput;
}

int ScreenWindow::scrollCount() const
{
    return _scrollCount;
}

void ScreenWindow::resetScrollCount() 
{
    _scrollCount = 0;
}

QRect ScreenWindow::scrollRegion() const
{
	bool equalToScreenSize = windowLines() == _screen->getLines();

	if ( atEndOfOutput() && equalToScreenSize )
    	return _screen->lastScrolledRegion();
	else
		return QRect(0,0,windowColumns(),windowLines());
}

void ScreenWindow::notifyOutputChanged()
{
    // move window to the bottom of the screen and update scroll count
    // if this window is currently tracking the bottom of the screen
    if ( _trackOutput )
    { 
        _scrollCount -= _screen->scrolledLines();
        _currentLine = qMax(0,_screen->getHistLines() - (windowLines()-_screen->getLines()));
    }
    else
    {
        // if the history is not unlimited then it may 
        // have run out of space and dropped the oldest
        // lines of output - in this case the screen
        // window's current line number will need to 
        // be adjusted - otherwise the output will scroll
        _currentLine = qMax(0,_currentLine - 
                              _screen->droppedLines());

        // ensure that the screen window's current position does
        // not go beyond the bottom of the screen
        _currentLine = qMin( _currentLine , _screen->getHistLines() );
    }

	_bufferNeedsUpdate = true;

    emit outputChanged(); 
}
void ScreenWindow::createFilterFolds(const QString& filter)
{
	removeAllFolds();

	if (filter.isEmpty())
		return;

	const int count = lineCount();
	const int lastLine = count - 1;
	const int firstLine = 0;
	const int columns = windowColumns();

	Character buffer[columns];
	QString lineText(columns,' ');
	int foldStart = -1;

	for (int i=0;i<count;i++)
	{
		_screen->getImage(buffer,columns,i,i);
	
		for (int j=0 ; j < columns ; j++)
			lineText[j] = QChar(buffer[j].character);

		//qDebug() << "Looking for" << filter << "in" << lineText.simplified();
		bool match = lineText.contains(filter);

		//if (match)
		//	qDebug() << "Match for line: '" << lineText.simplified() << "'";
		
		if (match || i == firstLine || i == lastLine)
		{
			// create the fold for the previous match
			if (i != firstLine)
				setFold(foldStart,i-1,true);

			// set the start for the next fold to the current line
			foldStart = i;
		}
	}
}

#include "ScreenWindow.moc"
