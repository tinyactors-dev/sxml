<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>

  <!-- Index the books of the library by their id. -->
  <xsl:key name="book" match="book" use="@id"/>
  <xsl:variable name="library" select="doc('library.xml')"/>

  <xsl:template match="/loans">
    <xsl:for-each select="loan">
      <xsl:text>{@date}  {@member}  {key('book', @book, $library)/title}&#10;</xsl:text>
    </xsl:for-each>
  </xsl:template>
</xsl:stylesheet>
